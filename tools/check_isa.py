"""Refuse instructions above AMD K10 in the K10 build: the app, the game code library and the bundled libraries.

AMD family 10h (Phenom II X4/X6, Athlon II) has SSE3, SSE4a, POPCNT and LZCNT, but none of SSSE3, SSE4.1, SSE4.2, AVX
or BMI. -march=amdfam10 keeps compiled code inside that set, but inline assembly, target attributes and intrinsics pass
through, and a prebuilt library can carry anything. A function that holds such an instruction behind a runtime CPU check
goes in DISPATCHED (a regular expression over its name) with that check named. Libraries that choose their SIMD code by
CPUID at run time on their own (SDL3, FFmpeg) are scanned with --report-only.

    python tools/check_isa.py [--objdump PATH] [--report-only] FILE...   (objects, archives, executables, libraries)

Adapted from TotK PC Recomp's tools/check_isa.py (MIT, Copyright (c) 2026 Valiantsin Dzerakh), by way of the Savage Wind
Recomp K10 fork. Standard library only.
"""
import argparse
import collections
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

# Mnemonics outside x86-64-v2 that are not VEX/EVEX encoded, or VEX encoded but without a v prefix (BMI1/2).
# lzcnt and tzcnt decode as bsr and bsf on older CPUs and give other results; tzcnt is left out because the
# compiler emits it on any x86-64 level where bsf would do (a nonzero input, or one it checks first).
NON_V2 = {'andn', 'bextr', 'blsi', 'blsmsk', 'blsr', 'bzhi', 'shlx', 'shrx', 'sarx', 'rorx', 'pdep', 'pext', 'mulx',
          'lzcnt', 'movbe', 'adcx', 'adox', 'sha1rnds4', 'sha1nexte', 'sha1msg1', 'sha1msg2', 'sha256rnds2',
          'sha256msg1', 'sha256msg2', 'aesenc', 'aesenclast', 'aesdec', 'aesdeclast', 'aesimc', 'aeskeygenassist',
          'pclmulqdq', 'rdrand', 'rdseed'}
# SSSE3, SSE4.1 and SSE4.2 (all inside x86-64-v2), which AMD K10 lacks. pextrw/pinsrw with a register operand are SSE2
# and stay allowed; the SSE4.1 memory form of pextrw is rare enough to leave out.
NON_K10 = (NON_V2 - {'lzcnt'}) | {
    'pshufb', 'phaddw', 'phaddd', 'phaddsw', 'phsubw', 'phsubd', 'phsubsw', 'pmaddubsw', 'pmulhrsw', 'psignb',
    'psignw', 'psignd', 'pabsb', 'pabsw', 'pabsd', 'palignr',
    'blendps', 'blendpd', 'blendvps', 'blendvpd', 'pblendw', 'pblendvb', 'roundps', 'roundpd', 'roundss', 'roundsd',
    'dpps', 'dppd', 'insertps', 'extractps', 'mpsadbw', 'phminposuw', 'pmulld', 'pmuldq', 'pminsb', 'pminsd',
    'pminuw', 'pminud', 'pmaxsb', 'pmaxsd', 'pmaxuw', 'pmaxud', 'pextrb', 'pextrd', 'pextrq', 'pinsrb', 'pinsrd',
    'pinsrq', 'ptest', 'packusdw', 'pcmpeqq', 'movntdqa',
    'pmovsxbw', 'pmovsxbd', 'pmovsxbq', 'pmovsxwd', 'pmovsxwq', 'pmovsxdq',
    'pmovzxbw', 'pmovzxbd', 'pmovzxbq', 'pmovzxwd', 'pmovzxwq', 'pmovzxdq',
    'pcmpgtq', 'crc32', 'crc32b', 'crc32w', 'crc32l', 'crc32q', 'pcmpestri', 'pcmpestrm', 'pcmpistri', 'pcmpistrm'}
LEVEL_SET = {'v2': NON_V2, 'k10': NON_K10}
LEVEL_NAME = {'v2': 'x86-64-v2', 'k10': 'AMD K10 (SSE3/SSE4a)'}
PREFIXES = {'rep', 'repe', 'repz', 'repne', 'repnz', 'lock', 'notrack', 'data16', 'cs', 'ds', 'es', 'ss', 'fs', 'gs'}
WIDE = re.compile(r'%[yz]mm\d|%k[0-7]\b')

# Function -> the runtime check that keeps it off CPUs without the instructions. None in Power Moon Recomp's own code
# yet; an entry looks like  r'avx2_\w+': 'runtime/x.c have_avx2: __builtin_cpu_supports("avx2")'.
DISPATCHED = {}
DISPATCHED_RE = re.compile('|'.join(f'(?:{k})' for k in DISPATCHED) + r'\Z' if DISPATCHED else r'(?!)')


def find_objdump(hint=None):
    """llvm-objdump next to the compiler in use, else on PATH, else GNU objdump; None when there is none."""
    for p in ([Path(hint)] if hint else []):
        if p.is_file():
            return str(p)
    for name in ('llvm-objdump', 'objdump'):
        found = shutil.which(name)
        if found:
            return found
    return None


def mnemonic(line):
    """The mnemonic of one disassembly line (llvm-objdump or GNU objdump, AT&T syntax), or None."""
    s = line.strip()
    m = re.match(r'^[0-9a-f]+:\s*', s)
    if m:
        s = s[m.end():]
    words = s.split()
    while words and words[0] in PREFIXES:
        words = words[1:]
    return words[0] if words else None


def strip_suffix(mn):
    """AT&T size suffixes (crc32l, pextrdl...) are not in the sets; try the bare mnemonic too."""
    return mn[:-1] if mn[-1:] in 'bwlq' else mn


def is_non_v2(line, mn, level='v2'):
    if mn.startswith('v') and mn not in ('verr', 'verw'):
        return True   # VEX or EVEX: AVX, AVX2, FMA, F16C, AVX-512
    if mn.startswith('k') and WIDE.search(line):
        return True   # AVX-512 mask registers
    bad = LEVEL_SET[level]
    return mn in bad or strip_suffix(mn) in bad or bool(WIDE.search(line))


def scan_one(path, objdump, level='v2'):
    """{(file, function): Counter of mnemonics} of one file."""
    hits = collections.defaultdict(collections.Counter)
    r = subprocess.run([objdump, '-d', '--no-show-raw-insn', str(path)], capture_output=True, text=True,
                       errors='replace')
    if r.returncode:
        sys.exit(f'check_isa: {objdump} failed on {path}: {r.stderr.strip()[:300]}')
    obj = str(path)
    func = None
    for line in r.stdout.splitlines():
        if 'file format' in line:
            obj = line.split(':\tfile format')[0].split(':     file format')[0].strip()
            continue
        m = re.match(r'^[0-9a-f]*\s*<(.+)>:$', line)
        if m:
            # ThinLTO renames a promoted static to <name>.llvm.<hash>; DISPATCHED names the source function
            func = re.sub(r'\.llvm\.\d+$', '', m.group(1))
            continue
        if not line[:1].isspace():
            continue
        mn = mnemonic(line)
        if mn and not (func and DISPATCHED_RE.match(func)) and is_non_v2(line, mn, level):
            hits[(obj, func)][mn] += 1
    return hits


def scan(paths, objdump, level='v2'):
    """{(file, function): Counter of mnemonics} for every function outside DISPATCHED with an instruction above level."""
    hits = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=min(16, os.cpu_count() or 1)) as pool:
        for h in pool.map(lambda p: scan_one(p, objdump, level), paths):
            hits.update(h)
    return hits


def report(hits, level='v2'):
    lines = [f'  {f} in {o}: ' + ', '.join(f'{k} x{v}' for k, v in c.most_common(6))
             for (o, f), c in sorted(hits.items())]
    if level == 'k10':
        return ('instructions above AMD K10 in the K10 build (a Phenom II stops on SSSE3/SSE4.1/SSE4.2/AVX with an '
                'illegal instruction); use SSE2/SSE3 code under #ifndef __SSSE3__, or a runtime CPU check and an entry '
                'in tools/check_isa.py DISPATCHED:\n' + '\n'.join(lines))
    return ('instructions above x86-64-v2 (a CPU without AVX/BMI stops on them with an illegal '
            'instruction); use SSE4.2 code under #ifndef __AVX__, or a runtime CPU check and an entry in '
            'tools/check_isa.py DISPATCHED:\n' + '\n'.join(lines))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('files', nargs='+', type=Path)
    ap.add_argument('--objdump', help='llvm-objdump or objdump to use (default: PATH)')
    ap.add_argument('--level', choices=('v2', 'k10'), default='k10', help='CPU level to check against (default k10)')
    ap.add_argument('--report-only', action='store_true', help='list what is above the level, but pass')
    args = ap.parse_args()
    objdump = find_objdump(args.objdump)
    if not objdump:
        sys.exit('check_isa: no llvm-objdump or objdump found')
    hits = scan(args.files, objdump, args.level)
    if hits:
        text = report(hits, args.level)
        if not args.report_only:
            sys.exit(text)
        print(f'check_isa (report only): {len(hits)} functions above {LEVEL_NAME[args.level]}')
        return
    print(f'check_isa: {len(args.files)} files, no instructions above {LEVEL_NAME[args.level]}')


if __name__ == '__main__':
    main()
