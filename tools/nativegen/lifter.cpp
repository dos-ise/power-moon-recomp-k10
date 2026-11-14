#include "lifter.h"

#include <cstdio>
#include <map>
#include <set>
#include <sstream>

#include "simd_c.h"

namespace ngen {

static const int SCRATCH[9] = {9, 10, 11, 12, 13, 14, 15, 16, 17};
static const char *COND_NAMES[16] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
                                     "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"};

void decode_bit_masks(int N, int imms, int immr, int size, bool immediate, uint64_t &wmask,
                      uint64_t &tmask) {
    int x = (N << 6) | (~imms & 0x3F);
    if (x == 0) throw Unsupported("reserved bitmask");
    int length = 0;
    for (int t = x; t; t >>= 1) length++;
    length--;
    if (length < 1) throw Unsupported("reserved bitmask");
    uint64_t levels = ones(length);
    if (immediate && (imms & levels) == levels) throw Unsupported("reserved bitmask");
    int S = imms & (int)levels;
    int R = immr & (int)levels;
    int diff = (S - R) & 0x3F;
    int esize = 1 << length;
    int d = diff & (int)ones(length);
    uint64_t welem = ones(S + 1);
    uint64_t telem = ones(d + 1);
    wmask = replicate(ror(welem, R, esize), esize, size);
    tmask = replicate(telem, esize, size);
}

static std::string cond_expr(int cond) {
    if (cond < 14) {
        std::string n = COND_NAMES[cond];
        for (auto &c : n) c = (char)toupper(c);
        return "COND_" + n + "(c)";
    }
    return "1";
}

static bool is_fp_simd_dp(uint32_t w) { return bits(w, 25, 3) == 0b111; }
static bool is_ldst(uint32_t w) { return bits(w, 25, 1) == 0 && bits(w, 27, 1) == 1; }

Lifter::Lifter(uint64_t base, const std::string &simd) : base(base) {
    if (simd == "c") sc = new SimdC();
    else if (simd != "asm") throw Unsupported("simd=" + simd);
}

Insn Lifter::lift(uint32_t w, uint64_t pc) {
    uint32_t op0 = bits(w, 25, 4);
    if ((op0 & 0b1110) == 0b1000) return dp_imm(w, pc);
    if ((op0 & 0b1110) == 0b1010) return branch_sys(w, pc);
    if ((op0 & 0b0101) == 0b0100) return ldst(w, pc);
    if ((op0 & 0b0111) == 0b0101) return dp_reg(w, pc);
    if ((op0 & 0b0111) == 0b0111) return fp_simd(w, pc);
    Insn i;
    i.kind = Insn::Trap;
    i.code = "rt_udf(c, " + hx(pc) + ");";
    return i;
}

// ------------------------------------------------------------ data processing (imm)

Insn Lifter::dp_imm(uint32_t w, uint64_t pc) {
    int op = (int)bits(w, 23, 3);
    bool sf = bits(w, 31, 1);
    int rd = (int)bits(w, 0, 5), rn = (int)bits(w, 5, 5);
    Insn i;
    i.kind = Insn::Op;
    if (op == 0 || op == 1) {  // PC-relative
        int64_t imm = sext((bits(w, 5, 19) << 2) | bits(w, 29, 2), 21);
        uint64_t val;
        if (bits(w, 31, 1)) val = (pc & ~0xFFFull) + (uint64_t)(imm << 12);
        else val = pc + (uint64_t)imm;
        i.code = set_reg(true, rd, hx(val & ones(64)));
        return i;
    }
    if (op == 2) {  // add/sub imm
        int opc = (int)bits(w, 30, 1), S = (int)bits(w, 29, 1), sh = (int)bits(w, 22, 1);
        uint64_t imm = (uint64_t)bits(w, 10, 12) << (12 * sh);
        std::string a = R(sf, rn, true);
        if (S) {
            std::string fn = std::string(opc ? "subs" : "adds") + (sf ? "64" : "32");
            i.code = set_reg(sf, rd, fn + "(c, " + a + ", " + hx(imm) + ")");
        } else {
            i.code = set_reg(sf, rd, a + (opc ? " - " : " + ") + hx(imm), true);
        }
        return i;
    }
    if (op == 4) {  // logical imm
        int opc = (int)bits(w, 29, 2), N = (int)bits(w, 22, 1);
        if (!sf && N) throw Unsupported("logical imm N=1 32-bit");
        int size = sf ? 64 : 32;
        uint64_t imm, tmask;
        decode_bit_masks(N, (int)bits(w, 10, 6), (int)bits(w, 16, 6), size, true, imm, tmask);
        std::string a = R(sf, rn);
        if (opc == 0) i.code = set_reg(sf, rd, a + " & " + hx(imm), true);
        else if (opc == 1) i.code = set_reg(sf, rd, a + " | " + hx(imm), true);
        else if (opc == 2) i.code = set_reg(sf, rd, a + " ^ " + hx(imm), true);
        else i.code = set_reg(sf, rd, "logic" + std::to_string(size) + "(c, " + a + " & " + hx(imm) + ")");
        return i;
    }
    if (op == 5) {  // move wide
        int opc = (int)bits(w, 29, 2), hw = (int)bits(w, 21, 2);
        if (!sf && hw > 1) throw Unsupported("movw hw");
        uint64_t imm = (uint64_t)bits(w, 5, 16) << (hw * 16);
        if (opc == 0) i.code = set_reg(sf, rd, hx(~imm & ones(sf ? 64 : 32)));
        else if (opc == 2) i.code = set_reg(sf, rd, hx(imm));
        else if (opc == 3) {
            uint64_t mask = ~(0xFFFFull << (hw * 16)) & ones(64);
            i.code = set_reg(sf, rd, "(" + X(rd) + " & " + hx(mask) + ") | " + hx(imm));
        } else throw Unsupported("movw opc");
        return i;
    }
    if (op == 6) {  // bitfield
        int opc = (int)bits(w, 29, 2), N = (int)bits(w, 22, 1);
        int immr = (int)bits(w, 16, 6), imms = (int)bits(w, 10, 6);
        int size = sf ? 64 : 32;
        if (N != sf) throw Unsupported("bitfield N");
        uint64_t wmask, tmask;
        decode_bit_masks(N, imms, immr, size, false, wmask, tmask);
        std::string src = R(sf, rn);
        std::string rot = immr ? "ROR" + std::to_string(size) + "(" + src + ", " + std::to_string(immr) + ")"
                               : src;
        if (opc == 2) {  // UBFM
            i.code = set_reg(sf, rd, rot + " & " + hx(wmask & tmask));
        } else if (opc == 0) {  // SBFM
            std::string top = "(uint" + std::to_string(size) + "_t)(-(int" + std::to_string(size) +
                              "_t)((" + src + " >> " + std::to_string(imms) + ") & 1))";
            i.code = set_reg(sf, rd, "(" + top + " & " + hx(~tmask & ones(size)) + ") | (" + rot +
                                       " & " + hx(wmask & tmask) + ")");
        } else if (opc == 1) {  // BFM
            std::string dst = R(sf, rd);
            std::string bot = "((" + dst + " & " + hx(~wmask & ones(size)) + ") | (" + rot + " & " +
                              hx(wmask) + "))";
            i.code = set_reg(sf, rd, "(" + dst + " & " + hx(~tmask & ones(size)) + ") | (" + bot +
                                       " & " + hx(tmask) + ")");
        } else throw Unsupported("bitfield opc");
        return i;
    }
    if (op == 7) {  // EXTR
        int rm = (int)bits(w, 16, 5), lsb = (int)bits(w, 10, 6);
        int size = sf ? 64 : 32;
        if (lsb == 0) i.code = set_reg(sf, rd, R(sf, rm));
        else i.code = set_reg(sf, rd, "(" + R(sf, rm) + " >> " + std::to_string(lsb) + ") | ((uint" +
                                          std::to_string(size) + "_t)" + R(sf, rn) + " << " +
                                          std::to_string(size - lsb) + ")");
        return i;
    }
    throw Unsupported("dp_imm");
}

// ------------------------------------------------------------ branches / system

Insn Lifter::branch_sys(uint32_t w, uint64_t pc) {
    Insn i;
    if (bits(w, 26, 5) == 0b00101) {  // B / BL
        uint64_t t = (pc + (uint64_t)sext(bits(w, 0, 26), 26) * 4) & ones(64);
        i.kind = bits(w, 31, 1) ? Insn::Bl : Insn::B;
        i.target = t;
        return i;
    }
    if (bits(w, 25, 7) == 0b0101010 && !bits(w, 4, 1)) {  // B.cond
        uint64_t t = pc + (uint64_t)sext(bits(w, 5, 19), 19) * 4;
        int cond = (int)bits(w, 0, 4);
        if (cond >= 14) {
            i.kind = Insn::B;
            i.target = t;
        } else {
            i.kind = Insn::Bcond;
            i.target = t;
            i.code = cond_expr(cond);
        }
        return i;
    }
    if (bits(w, 25, 6) == 0b011010) {  // CBZ/CBNZ
        bool sf = bits(w, 31, 1), nz = bits(w, 24, 1);
        int rt = (int)bits(w, 0, 5);
        uint64_t t = pc + (uint64_t)sext(bits(w, 5, 19), 19) * 4;
        i.kind = Insn::Bcond;
        i.target = t;
        i.code = R(sf, rt) + (nz ? " != " : " == ") + "0";
        return i;
    }
    if (bits(w, 25, 6) == 0b011011) {  // TBZ/TBNZ
        int b = (int)((bits(w, 31, 1) << 5) | bits(w, 19, 5));
        bool nz = bits(w, 24, 1);
        int rt = (int)bits(w, 0, 5);
        uint64_t t = pc + (uint64_t)sext(bits(w, 5, 14), 14) * 4;
        i.kind = Insn::Bcond;
        i.target = t;
        i.code = "((" + X(rt) + " >> " + std::to_string(b) + ") & 1) " + (nz ? "!= " : "== ") + "0";
        return i;
    }
    if (bits(w, 25, 7) == 0b1101011) {  // branch register
        int opc = (int)bits(w, 21, 4), rn = (int)bits(w, 5, 5);
        if (bits(w, 16, 5) != 31 || bits(w, 10, 6) || bits(w, 0, 5)) throw Unsupported("branch reg variant");
        if (opc == 0) { i.kind = Insn::Br; i.target_expr = X(rn); }
        else if (opc == 1) { i.kind = Insn::Blr; i.target_expr = X(rn); }
        else if (opc == 2) { i.kind = Insn::Ret; }
        else throw Unsupported("branch reg opc");
        return i;
    }
    if (bits(w, 24, 8) == 0b11010100) {  // exception generation
        int opc = (int)bits(w, 21, 3), ll = (int)bits(w, 0, 5), imm = (int)bits(w, 5, 16);
        if (opc == 0 && ll == 1) {
            i.kind = Insn::Op;
            i.code = "rt_svc(c, " + std::to_string(imm) + ", " + hx(pc) + ");";
        } else if (opc == 1 && ll == 0) {
            i.kind = Insn::Trap;
            i.code = "rt_brk(c, " + std::to_string(imm) + ", " + hx(pc) + ");";
        } else throw Unsupported("exception");
        return i;
    }
    if (bits(w, 22, 10) == 0b1101010100) return system(w, pc);
    throw Unsupported("branch_sys");
}

Insn Lifter::system(uint32_t w, uint64_t pc) {
    (void)pc;
    bool L = bits(w, 21, 1);
    int op0 = (int)bits(w, 19, 2), op1 = (int)bits(w, 16, 3), crn = (int)bits(w, 12, 4),
        crm = (int)bits(w, 8, 4), op2 = (int)bits(w, 5, 3), rt = (int)bits(w, 0, 5);
    Insn i;
    i.kind = Insn::Op;
    if (op0 == 0 && !L) {
        if (crn == 2 && op1 == 3 && rt == 31) {  // hints
            i.code = "/* hint */";
            return i;
        }
        if (crn == 3 && op1 == 3 && rt == 31) {  // barriers
            if (op2 == 2) i.code = "c->excl_addr = ~0ull;";
            else i.code = "__atomic_thread_fence(__ATOMIC_SEQ_CST);";
            return i;
        }
        throw Unsupported("pstate/system");
    }
    if (op0 == 1) {  // SYS
        if (!L && op1 == 3 && crn == 7 && crm == 4 && op2 == 1)  // DC ZVA
            i.code = "memset((void*)(uintptr_t)(" + X(rt) + " & ~63ull), 0, 64);";
        else i.code = "/* cache maintenance */";
        return i;
    }
    struct Key { int a, b, c, d, e; };
    Key key{op0, op1, crn, crm, op2};
    if (L) {  // MRS
        std::string src;
        if (key.a == 3 && key.b == 3 && key.c == 13 && key.d == 0 && key.e == 3) src = "c->tpidrro";
        else if (key.a == 3 && key.b == 3 && key.c == 13 && key.d == 0 && key.e == 2) src = "c->tpidr";
        else if (key.a == 3 && key.b == 3 && key.c == 14 && key.d == 0 && key.e == 0) src = "19200000ull";
        else if (key.a == 3 && key.b == 3 && key.c == 14 && key.d == 0 && key.e == 1) src = "rt_ticks()";
        else if (key.a == 3 && key.b == 3 && key.c == 14 && key.d == 0 && key.e == 2) src = "rt_ticks()";
        else if (key.a == 3 && key.b == 3 && key.c == 4 && key.d == 2 && key.e == 0) src = "c->nzcv";
        else if (key.a == 3 && key.b == 3 && key.c == 4 && key.d == 4 && key.e == 0) src = sc ? "c->fpcr" : "rt_get_fpcr()";
        else if (key.a == 3 && key.b == 3 && key.c == 4 && key.d == 4 && key.e == 1) src = sc ? "c->fpsr" : "rt_get_fpsr()";
        else if (key.a == 3 && key.b == 3 && key.c == 0 && key.d == 0 && key.e == 7) src = "4ull";
        else if (key.a == 3 && key.b == 3 && key.c == 0 && key.d == 0 && key.e == 1) src = "0x8444c004ull";
        if (src.empty()) throw Unsupported("mrs");
        i.code = set_reg(true, rt, src);
        return i;
    }
    std::string dst;
    if (key.a == 3 && key.b == 3 && key.c == 13 && key.d == 0 && key.e == 2) dst = "c->tpidr = {v};";
    else if (key.a == 3 && key.b == 3 && key.c == 4 && key.d == 2 && key.e == 0) dst = "c->nzcv = {v} & 0xf0000000ull;";
    else if (key.a == 3 && key.b == 3 && key.c == 4 && key.d == 4 && key.e == 0)
        dst = sc ? "c->fpcr = {v} & 0x7c00000ull;" : "rt_set_fpcr({v});";
    else if (key.a == 3 && key.b == 3 && key.c == 4 && key.d == 4 && key.e == 1)
        dst = sc ? "c->fpsr = {v} & 0xf800009full;" : "rt_set_fpsr({v});";
    if (dst.empty()) throw Unsupported("msr");
    std::string v = X(rt);
    size_t p = dst.find("{v}");
    dst.replace(p, 3, v);
    i.code = dst;
    return i;
}

// ------------------------------------------------------------ loads / stores

Insn Lifter::ldst(uint32_t w, uint64_t pc) {
    bool V = bits(w, 26, 1);
    if (V) {
        if (sc) {
            Insn i;
            i.kind = Insn::Op;
            i.code = sc->ldst(w, pc);
            return i;
        }
        return ldst_simd(w, pc);
    }
    if (bits(w, 24, 6) == 0b001000) return ldst_excl(w, pc);
    if (bits(w, 27, 3) == 0b011 && bits(w, 24, 2) == 0) {  // literal
        int opc = (int)bits(w, 30, 2), rt = (int)bits(w, 0, 5);
        uint64_t addr = pc + (uint64_t)sext(bits(w, 5, 19), 19) * 4;
        Insn i;
        i.kind = Insn::Op;
        if (opc == 0) i.code = set_reg(false, rt, "LD32(" + hx(addr) + ")");
        else if (opc == 1) i.code = set_reg(true, rt, "LD64(" + hx(addr) + ")");
        else if (opc == 2) i.code = set_reg(true, rt, "(int64_t)(int32_t)LD32(" + hx(addr) + ")");
        else i.code = "/* prfm */";
        return i;
    }
    if (bits(w, 27, 3) == 0b101) return ldst_pair(w);
    if (bits(w, 27, 3) == 0b111) return ldst_reg(w);
    throw Unsupported("ldst");
}

Insn Lifter::ldst_excl(uint32_t w, uint64_t pc) {
    (void)pc;
    int size = (int)bits(w, 30, 2), o2 = (int)bits(w, 23, 1), L = (int)bits(w, 22, 1),
        o1 = (int)bits(w, 21, 1), rs = (int)bits(w, 16, 5), rn = (int)bits(w, 5, 5),
        rt = (int)bits(w, 0, 5);
    int nbits = 8 << size;
    std::string addr = XS(rn);
    if (o1) throw Unsupported("exclusive pair / CAS");
    Insn i;
    i.kind = Insn::Op;
    if (o2 == 0) {
        if (L) i.code = set_reg(true, rt, "ldx" + std::to_string(nbits) + "(c, " + addr + ")");
        else i.code = "{ uint32_t st = stx" + std::to_string(nbits) + "(c, " + addr + ", " + X(rt) +
                      "); " + set_reg(false, rs, "st") + " }";
    } else {
        if (L) i.code = set_reg(true, rt, "lda" + std::to_string(nbits) + "(" + addr + ")");
        else i.code = "stl" + std::to_string(nbits) + "(" + addr + ", " + X(rt) + ");";
    }
    return i;
}

Insn Lifter::ldst_pair(uint32_t w) {
    int opc = (int)bits(w, 30, 2), mode = (int)bits(w, 23, 2), L = (int)bits(w, 22, 1);
    int64_t imm7 = sext(bits(w, 15, 7), 7);
    int rt2 = (int)bits(w, 10, 5), rn = (int)bits(w, 5, 5), rt = (int)bits(w, 0, 5);
    if (opc == 3) throw Unsupported("pair opc 3");
    int scale = opc == 2 ? 3 : 2;
    int64_t off = imm7 << scale;
    std::string base = XS(rn);
    bool post = mode == 1;
    bool wb = mode == 1 || mode == 3;
    std::string ea = post ? base
                          : (off >= 0 ? base + " + " + std::to_string(off) : base + " - " + std::to_string(-off));
    std::vector<std::string> lines;
    lines.push_back("uint64_t ea = " + ea + ";");
    if (L) {
        if (opc == 2) lines.push_back("uint64_t a = LD64(ea), b = LD64(ea + 8);");
        else if (opc == 1) lines.push_back("uint64_t a = (int64_t)(int32_t)LD32(ea), b = (int64_t)(int32_t)LD32(ea + 4);");
        else lines.push_back("uint64_t a = LD32(ea), b = LD32(ea + 4);");
    } else {
        if (opc == 2) lines.push_back("ST64(ea, " + X(rt) + "); ST64(ea + 8, " + X(rt2) + ");");
        else lines.push_back("ST32(ea, " + W(rt) + "); ST32(ea + 4, " + W(rt2) + ");");
    }
    if (wb) {
        std::string dst = rn == 31 ? "c->sp" : "c->x[" + std::to_string(rn) + "]";
        if (!post) lines.push_back(dst + " = ea;");
        else lines.push_back(off >= 0 ? dst + " = ea + " + std::to_string(off) + ";"
                                      : dst + " = ea - " + std::to_string(-off) + ";");
    }
    if (L) {
        lines.push_back(set_reg(true, rt, "a"));
        lines.push_back(set_reg(true, rt2, "b"));
    }
    Insn i;
    i.kind = Insn::Op;
    std::string body;
    for (size_t k = 0; k < lines.size(); k++) body += (k ? " " : "") + lines[k];
    i.code = "{ " + body + " }";
    return i;
}

Insn Lifter::ldst_reg(uint32_t w) {
    int size = (int)bits(w, 30, 2), opc = (int)bits(w, 22, 2), rn = (int)bits(w, 5, 5),
        rt = (int)bits(w, 0, 5);
    std::string base = XS(rn);
    std::string wb;
    std::string ea;
    if (bits(w, 24, 2) == 1) {
        ea = base + " + " + hx(bits(w, 10, 12) << size);
    } else {
        int kind = (int)bits(w, 10, 2);
        if (bits(w, 21, 1)) {
            if (kind != 2) throw Unsupported("ldst reg atomic/pac");
            int rm = (int)bits(w, 16, 5), option = (int)bits(w, 13, 3), S = (int)bits(w, 12, 1);
            if (option != 2 && option != 3 && option != 6 && option != 7)
                throw Unsupported("ldst reg option");
            ea = base + " + " + extend_expr(rm, option, S ? size : 0);
        } else {
            int64_t imm9 = sext(bits(w, 12, 9), 9);
            std::string sign_imm = imm9 >= 0 ? "+ " + std::to_string(imm9) : "- " + std::to_string(-imm9);
            if (kind == 0 || kind == 2) {  // unscaled, unprivileged
                ea = base + " " + sign_imm;
            } else if (kind == 1) {  // post
                ea = base;
                wb = base + " " + sign_imm;
            } else {  // pre
                ea = base + " " + sign_imm;
                wb = "ea";
            }
        }
    }
    int nbits = 8 << size;
    std::vector<std::string> parts;
    std::string load;
    if (opc == 0) {
        parts.push_back("ST" + std::to_string(nbits) + "(ea, " + (size == 3 ? X(rt) : W(rt)) + ");");
    } else if (opc == 1) {
        parts.push_back("uint64_t v = LD" + std::to_string(nbits) + "(ea);");
        load = set_reg(true, rt, "v");
    } else if (size == 3) {
        parts.push_back("/* prfm */");
    } else if (size == 2 && opc == 3) {
        throw Unsupported("ldst size2 opc3");
    } else {
        parts.push_back("uint64_t v = (uint64_t)(int64_t)(int" + std::to_string(nbits) + "_t)LD" +
                        std::to_string(nbits) + "(ea);");
        load = set_reg(opc == 2, rt, "v");
    }
    parts.insert(parts.begin(), "uint64_t ea = " + ea + ";");
    if (!wb.empty()) parts.push_back((rn == 31 ? "c->sp" : "c->x[" + std::to_string(rn) + "]") + " = " + wb + ";");
    if (!load.empty()) parts.push_back(load);
    Insn i;
    i.kind = Insn::Op;
    std::string body;
    for (size_t k = 0; k < parts.size(); k++) body += (k ? " " : "") + parts[k];
    i.code = "{ " + body + " }";
    return i;
}

Insn Lifter::ldst_simd(uint32_t w, uint64_t pc) {
    int rt = (int)bits(w, 0, 5);
    AsmInsn a;
    a.word = w;
    if (bits(w, 27, 3) == 0b011 && bits(w, 24, 2) == 0) {  // literal -> LDR (imm, offset 0) via scratch
        int opc = (int)bits(w, 30, 2);
        int size, o;
        if (opc == 0) { size = 2; o = 1; }
        else if (opc == 1) { size = 3; o = 1; }
        else { size = 0; o = 3; }
        uint64_t addr = pc + (uint64_t)sext(bits(w, 5, 19), 19) * 4;
        uint32_t nw = ((uint32_t)size << 30) | 0x3D000000u | ((uint32_t)o << 22) | (31u << 5) | (uint32_t)rt;
        a.word = nw;
        a.vregs.insert(rt);
        a.consts.push_back({5, addr});
    } else if (bits(w, 31, 1) == 0 && (bits(w, 24, 6) == 0b001100 || bits(w, 24, 6) == 0b001101)) {
        // structures
        bool post = bits(w, 23, 1);
        int rn = (int)bits(w, 5, 5), rm = (int)bits(w, 16, 5);
        int n;
        if (bits(w, 24, 1) == 0) {
            static const int map[16] = {4, -1, 4, -1, 3, -1, 3, 1, 2, -1, 2, -1, -1, -1, -1, -1};
            n = map[bits(w, 12, 4)];
            if (n < 0) throw Unsupported("ldst multiple opcode");
        } else {
            n = (int)((bits(w, 13, 1) << 1) | bits(w, 21, 1)) + 1;
        }
        for (int k = 0; k < n; k++) a.vregs.insert((rt + k) % 32);
        a.gprs.push_back({5, rn == 31 ? "sp" : std::to_string(rn)});
        if (post && rm != 31) a.gprs.push_back({16, std::to_string(rm)});
    } else {
        int rn = (int)bits(w, 5, 5);
        a.gprs.push_back({5, rn == 31 ? "sp" : std::to_string(rn)});
        a.vregs.insert(rt);
        if (bits(w, 27, 3) == 0b101) {
            a.vregs.insert((int)bits(w, 10, 5));
        } else if (bits(w, 27, 3) == 0b111 && bits(w, 24, 2) == 0 && bits(w, 21, 1) &&
                   bits(w, 10, 2) == 2) {
            int rm = (int)bits(w, 16, 5);
            if (rm != 31) a.gprs.push_back({16, std::to_string(rm)});
        }
    }
    Insn i;
    i.kind = Insn::Asm;
    i.asm_insn = std::move(a);
    return i;
}

// ------------------------------------------------------------ data processing (reg)

Insn Lifter::dp_reg(uint32_t w, uint64_t pc) {
    (void)pc;
    bool sf = bits(w, 31, 1);
    int rd = (int)bits(w, 0, 5), rn = (int)bits(w, 5, 5), rm = (int)bits(w, 16, 5);
    int size = sf ? 64 : 32;
    int op1 = (int)bits(w, 28, 1);
    Insn i;
    i.kind = Insn::Op;
    if (!op1) {
        if (bits(w, 24, 4) == 0b1010) {  // logical shifted
            int opc = (int)bits(w, 29, 2), sh = (int)bits(w, 22, 2), N = (int)bits(w, 21, 1),
                amt = (int)bits(w, 10, 6);
            if (!sf && amt >= 32) throw Unsupported("logical shift amount");
            std::string b = shift_expr(sf, R(sf, rm), sh, amt);
            if (N) b = "(uint" + std::to_string(size) + "_t)~" + b;
            std::string a = R(sf, rn);
            std::string expr;
            if (opc == 0) expr = a + " & " + b;
            else if (opc == 1) expr = a + " | " + b;
            else if (opc == 2) expr = a + " ^ " + b;
            else expr = a + " & " + b;
            if (opc == 3) i.code = set_reg(sf, rd, "logic" + std::to_string(size) + "(c, (uint" +
                                                      std::to_string(size) + "_t)(" + expr + "))");
            else i.code = set_reg(sf, rd, expr);
            return i;
        }
        if (bits(w, 24, 4) == 0b1011) {  // add/sub shifted or extended
            int op = (int)bits(w, 30, 1), S = (int)bits(w, 29, 1);
            std::string a, b;
            bool sp_ok = false;
            if (bits(w, 21, 1) == 0) {
                int sh = (int)bits(w, 22, 2), amt = (int)bits(w, 10, 6);
                if (sh == 3) throw Unsupported("add shift ror");
                a = R(sf, rn);
                b = shift_expr(sf, R(sf, rm), sh, amt);
            } else {
                if (bits(w, 22, 2)) throw Unsupported("add ext opt");
                int option = (int)bits(w, 13, 3), amt = (int)bits(w, 10, 3);
                a = R(sf, rn, true);
                b = extend_expr(rm, option, amt);
                if (!sf) b = "(uint32_t)(" + b + ")";
                sp_ok = !S;
            }
            if (S) {
                std::string fn = std::string(op ? "subs" : "adds") + std::to_string(size);
                i.code = set_reg(sf, rd, fn + "(c, " + a + ", (uint" + std::to_string(size) + "_t)(" + b + "))");
            } else {
                i.code = set_reg(sf, rd, a + (op ? " - " : " + ") + b, sp_ok);
            }
            return i;
        }
        throw Unsupported("dp_reg");
    }
    int op2 = (int)bits(w, 21, 4);
    if (bits(w, 24, 4) == 0b1011) {  // 3-source
        int op54 = (int)bits(w, 29, 2), op31 = (int)bits(w, 21, 3), o0 = (int)bits(w, 15, 1),
            ra = (int)bits(w, 10, 5);
        if (op54) throw Unsupported("dp3 op54");
        std::string sgn = o0 ? "-" : "+";
        if (op31 == 0) i.code = set_reg(sf, rd, R(sf, ra) + " " + sgn + " " + R(sf, rn) + " * " + R(sf, rm));
        else if (op31 == 1)
            i.code = set_reg(true, rd, X(ra) + " " + sgn +
                                        " (uint64_t)((int64_t)(int32_t)" + W(rn) + " * (int64_t)(int32_t)" + W(rm) + ")");
        else if (op31 == 5)
            i.code = set_reg(true, rd, X(ra) + " " + sgn + " (uint64_t)" + W(rn) + " * (uint64_t)" + W(rm));
        else if (op31 == 2)
            i.code = set_reg(true, rd, "(uint64_t)(((__int128)(int64_t)" + X(rn) + " * (__int128)(int64_t)" +
                                        X(rm) + ") >> 64)");
        else if (op31 == 6)
            i.code = set_reg(true, rd, "(uint64_t)(((unsigned __int128)" + X(rn) + " * " + X(rm) + ") >> 64)");
        else throw Unsupported("dp3");
        return i;
    }
    if (op2 == 0) {  // add/sub with carry
        int op = (int)bits(w, 30, 1), S = (int)bits(w, 29, 1);
        std::string fn = std::string(op ? "sbc" : "adc") + (S ? "s" : "") + std::to_string(size);
        i.code = set_reg(sf, rd, fn + "(c, " + R(sf, rn) + ", " + R(sf, rm) + ")");
        return i;
    }
    if (op2 == 2) {  // conditional compare
        int op = (int)bits(w, 30, 1), imm_form = (int)bits(w, 11, 1), cond = (int)bits(w, 12, 4),
            nzcv = (int)bits(w, 0, 4);
        std::string b = imm_form ? hx(rm) : R(sf, rm);
        std::string fn = std::string(op ? "subs" : "adds") + std::to_string(size);
        i.code = "if (" + cond_expr(cond) + ") (void)" + fn + "(c, " + R(sf, rn) + ", " + b + "); else c->nzcv = " +
                 hx((uint64_t)nzcv << 28) + ";";
        return i;
    }
    if (op2 == 4) {  // conditional select
        int op = (int)bits(w, 30, 1), o2 = (int)bits(w, 10, 2), cond = (int)bits(w, 12, 4);
        std::string b = R(sf, rm);
        std::string alt;
        if (op == 0 && o2 == 0) alt = b;
        else if (op == 0 && o2 == 1) alt = b + " + 1";
        else if (op == 1 && o2 == 0) alt = "~" + b;
        else if (op == 1 && o2 == 1) alt = "-" + b;
        else throw Unsupported("csel variant");
        i.code = set_reg(sf, rd, "(" + cond_expr(cond) + ") ? " + R(sf, rn) + " : (uint" +
                                    std::to_string(size) + "_t)(" + alt + ")");
        return i;
    }
    if (op2 == 6) {
        int opcode = (int)bits(w, 10, 6);
        if (bits(w, 30, 1)) {  // 1-source
            if (bits(w, 16, 5)) throw Unsupported("dp1 opcode2");
            std::string a = R(sf, rn);
            std::string expr;
            if (opcode == 0) expr = "rbit" + std::to_string(size) + "(" + a + ")";
            else if (opcode == 1) expr = "rev16_" + std::to_string(size) + "(" + a + ")";
            else if (opcode == 2) expr = sf ? "rev32_64(" + a + ")" : "__builtin_bswap32(" + a + ")";
            else if (opcode == 3) expr = sf ? "__builtin_bswap64(" + a + ")" : "";
            else if (opcode == 4) expr = "clz" + std::to_string(size) + "(" + a + ")";
            else if (opcode == 5) expr = "cls" + std::to_string(size) + "(" + a + ")";
            if (expr.empty()) throw Unsupported("dp1");
            i.code = set_reg(sf, rd, expr);
            return i;
        }
        if (opcode >= 16 && opcode <= 23) {  // CRC32
            if (sc) {
                i.code = sc->crc(w);
            } else {
                AsmInsn a;
                a.word = w;
                if (rd != 31) a.gprs.push_back({0, std::to_string(rd)});
                if (rn != 31) a.gprs.push_back({5, std::to_string(rn)});
                if (rm != 31) a.gprs.push_back({16, std::to_string(rm)});
                i.kind = Insn::Asm;
                i.asm_insn = std::move(a);
            }
            return i;
        }
        std::string a = R(sf, rn), b = R(sf, rm);
        std::string expr;
        if (opcode == 2) expr = "udiv" + std::to_string(size) + "(" + a + ", " + b + ")";
        else if (opcode == 3) expr = "sdiv" + std::to_string(size) + "(" + a + ", " + b + ")";
        else if (opcode == 8) expr = a + " << (" + b + " & " + std::to_string(size - 1) + ")";
        else if (opcode == 9) expr = a + " >> (" + b + " & " + std::to_string(size - 1) + ")";
        else if (opcode == 10)
            expr = "(uint" + std::to_string(size) + "_t)((int" + std::to_string(size) + "_t)" + a + " >> (" + b +
                   " & " + std::to_string(size - 1) + "))";
        else if (opcode == 11) expr = "ROR" + std::to_string(size) + "(" + a + ", " + b + " & " +
                                      std::to_string(size - 1) + ")";
        else throw Unsupported("dp2");
        i.code = set_reg(sf, rd, expr);
        return i;
    }
    throw Unsupported("dp_reg");
}

// ------------------------------------------------------------ FP / SIMD data processing

Insn Lifter::fp_simd(uint32_t w, uint64_t pc) {
    if (sc) {
        Insn i;
        i.kind = Insn::Op;
        i.code = sc->dp(w, pc);
        return i;
    }
    int rd = (int)bits(w, 0, 5), rn = (int)bits(w, 5, 5), rm = (int)bits(w, 16, 5),
        ra = (int)bits(w, 10, 5);
    std::set<int> vregs = {rd, rn, rm, ra};
    std::vector<std::pair<int, std::string>> gprs;
    bool flags = false;
    if ((w & 0x5F20FC00u) == 0x1E200000u) {  // FP <-> integer
        int opcode = (int)bits(w, 16, 3);
        if (opcode == 0 || opcode == 1 || opcode == 4 || opcode == 5 || opcode == 6) {
            gprs.push_back({0, std::to_string(rd)});
            vregs = {rn};
        } else {
            gprs.push_back({5, std::to_string(rn)});
            vregs = {rd};
        }
    } else if ((w & 0x5F200000u) == 0x1E000000u) {  // FP <-> fixed point
        int opcode = (int)bits(w, 16, 3);
        if (opcode == 0 || opcode == 1) {
            gprs.push_back({0, std::to_string(rd)});
            vregs = {rn};
        } else {
            gprs.push_back({5, std::to_string(rn)});
            vregs = {rd};
        }
    } else if ((w & 0x9FE08400u) == 0x0E000400u && bits(w, 29, 1) == 0) {  // AdvSIMD copy
        int imm4 = (int)bits(w, 11, 4);
        if (imm4 == 1 || imm4 == 3) {
            gprs.push_back({5, std::to_string(rn)});
            vregs = {rd};
        } else if (imm4 == 5 || imm4 == 7) {
            gprs.push_back({0, std::to_string(rd)});
            vregs = {rn};
        } else {
            vregs = {rd, rn};
        }
    } else if ((w & 0xBFE08C00u) == 0x0E000000u) {  // TBL/TBX
        int n = (int)bits(w, 13, 2) + 1;
        vregs = {rd, rm};
        for (int k = 0; k < n; k++) vregs.insert((rn + k) % 32);
    }
    if ((w & 0x5F203C00u) == 0x1E202000u || (w & 0x5F200C00u) == 0x1E200400u ||
        (w & 0x5F200C00u) == 0x1E200C00u)
        flags = true;
    AsmInsn a;
    a.word = w;
    a.vregs = std::move(vregs);
    a.flags = flags;
    for (auto &g : gprs)
        if (g.second != "31") a.gprs.push_back(g);
    Insn i;
    i.kind = Insn::Asm;
    i.asm_insn = std::move(a);
    return i;
}

std::string Lifter::emit_asm_batch(const std::vector<AsmInsn> &batch) {
    std::set<int> vregs;
    std::map<std::string, int> gmap;
    bool flags = false;
    std::vector<std::pair<int, uint64_t>> consts;
    std::vector<uint32_t> words;
    for (const AsmInsn &a : batch) {
        vregs.insert(a.vregs.begin(), a.vregs.end());
        flags = flags || a.flags;
        uint32_t w = a.word;
        for (auto &gp : a.gprs) {
            int pos = gp.first;
            const std::string &g = gp.second;
            if (!gmap.count(g)) {
                if ((int)gmap.size() + (int)consts.size() >= 9) throw Unsupported("scratch exhausted");
                gmap[g] = SCRATCH[gmap.size() + consts.size()];
            }
            w = (w & ~(31u << pos)) | ((uint32_t)gmap[g] << pos);
        }
        for (auto &cp : a.consts) {
            int pos = cp.first;
            int s = SCRATCH[gmap.size() + consts.size()];
            consts.push_back({s, cp.second});
            w = (w & ~(31u << pos)) | ((uint32_t)s << pos);
        }
        words.push_back(w);
    }
    std::vector<std::string> lines;
    std::vector<std::string> ins;
    ins.push_back("\"r\"(c)");
    for (auto &kv : gmap) {
        const std::string &g = kv.first;
        int s = kv.second;
        uint64_t off = g == "sp" ? OFF_SP : OFF_X + 8 * (uint64_t)std::stoll(g);
        lines.push_back("ldr x" + std::to_string(s) + ", [%0, #" + std::to_string(off) + "]");
    }
    for (size_t i = 0; i < consts.size(); i++) {
        ins.push_back("\"r\"(" + hx(consts[i].second) + ")");
        lines.push_back("mov x" + std::to_string(consts[i].first) + ", %" + std::to_string(ins.size() - 1));
    }
    for (int v : vregs) lines.push_back("ldr q" + std::to_string(v) + ", [%0, #" +
                                        std::to_string(OFF_V + 16 * (uint64_t)v) + "]");
    if (flags) {
        lines.push_back("ldr x16, [%0, #" + std::to_string(OFF_NZCV) + "]");
        lines.push_back("msr nzcv, x16");
    }
    for (uint32_t x : words) {
        char buf[24];
        snprintf(buf, sizeof buf, ".inst 0x%08x", x);
        lines.push_back(buf);
    }
    if (flags) {
        lines.push_back("mrs x16, nzcv");
        lines.push_back("str x16, [%0, #" + std::to_string(OFF_NZCV) + "]");
    }
    for (int v : vregs) lines.push_back("str q" + std::to_string(v) + ", [%0, #" +
                                        std::to_string(OFF_V + 16 * (uint64_t)v) + "]");
    for (auto &kv : gmap) {
        const std::string &g = kv.first;
        int s = kv.second;
        uint64_t off = g == "sp" ? OFF_SP : OFF_X + 8 * (uint64_t)std::stoll(g);
        lines.push_back("str x" + std::to_string(s) + ", [%0, #" + std::to_string(off) + "]");
    }
    std::set<int> scratch;
    for (auto &kv : gmap) scratch.insert(kv.second);
    for (auto &cp : consts) scratch.insert(cp.first);
    if (flags) scratch.insert(16);
    std::vector<std::string> clobber;
    for (int v : vregs) clobber.push_back("\"v" + std::to_string(v) + "\"");
    for (int s : scratch) clobber.push_back("\"x" + std::to_string(s) + "\"");
    clobber.push_back("\"cc\"");
    clobber.push_back("\"memory\"");
    std::string body;
    for (size_t k = 0; k < lines.size(); k++) body += (k ? "\\n\\t" : "") + lines[k];
    std::string ins_join, clob_join;
    for (size_t k = 0; k < ins.size(); k++) ins_join += (k ? ", " : "") + ins[k];
    for (size_t k = 0; k < clobber.size(); k++) clob_join += (k ? ", " : "") + clobber[k];
    return "__asm__ volatile(\"" + body + "\" :: " + ins_join + " : " + clob_join + ");";
}

}  // namespace ngen
