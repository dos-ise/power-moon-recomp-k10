#include "gen.h"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include "ehframe.h"
#include "patches.h"

namespace ngen {

static const uint64_t SCRATCH_MEM = 0x7300000000ull;

static std::string fname(uint64_t a) {
    char buf[24];
    snprintf(buf, sizeof buf, "f_%llx", (unsigned long long)a);
    return buf;
}
static std::string sanitize(const std::string &sym) {
    std::string s = "hle_";
    for (char c : sym)
        s += (isalnum((unsigned char)c) || c == '_') ? c : '_';
    return s;
}
static int64_t sext26(uint32_t w) {
    uint32_t v = w & 0x03FFFFFF;
    return (v & 0x02000000) ? (int64_t)v - 0x04000000 : (int64_t)v;
}

// ------------------------------------------------------------------ Gen

Gen::Gen(const std::string &name, const std::string &path, uint64_t base, const std::string &simd)
    : name(name), nso(path), base(base), lifter(base, simd) {
    lo = base + nso.text.first;
    hi = base + nso.text.first + nso.text.second;
    words.resize(nso.text.second / 4);
    memcpy(words.data(), nso.image.data() + nso.text.first, nso.text.second);
    std::string build = nso.build_id.substr(0, 16);
    auto &pb = patches_by_build();
    auto it = pb.find(build);
    if (it != pb.end()) patches = it->second;
    auto &hb = ngen::hle_by_build();
    auto it2 = hb.find(build);
    if (it2 != hb.end()) hle_extra = it2->second;
}

bool Gen::in_text(uint64_t a) const { return lo <= a && a < hi && a % 4 == 0; }
uint32_t Gen::word(uint64_t a) const { return words[(a - lo) / 4]; }

Insn Gen::lift(uint64_t a) {
    try {
        return lifter.lift(word(a), a);
    } catch (const Unsupported &e) {
        stats["unsupported"]++;
        Insn i;
        i.kind = Insn::Trap;
        i.code = "rt_udf(c, " + hx(a) + "); /* " + e.what() + " */";
        return i;
    }
}

std::vector<uint64_t> Gen::boundaries() {
    std::set<uint64_t> starts;
    for (uint64_t a : eh_frame_functions(nso)) starts.insert(base + a);
    for (auto &s : nso.syms)
        if (s.shndx && (s.info & 0xF) == 2) starts.insert(base + s.value);
    for (auto &r : nso.relocs)
        if (r.type == R_AARCH64_RELATIVE) starts.insert(base + (uint64_t)r.add);
    std::vector<uint64_t> out;
    for (uint64_t a : starts)
        if (in_text(a)) out.push_back(a);
    std::sort(out.begin(), out.end());
    return out;
}

Interp *Gen::emu() {
    if (!mu_) mu_ = new Interp(nso.image, base);
    return mu_;
}

// ---- jump tables

std::vector<uint64_t> Gen::jump_table(uint64_t br_pc, uint64_t entry, uint64_t end) {
    std::vector<uint64_t> r = _jump_table_fallthrough(br_pc, entry, end);
    if (r.empty()) r = _jump_table_taken(br_pc, entry, end);
    return r;
}

std::vector<int> Gen::_index_regs(uint64_t start, uint64_t at, int idx) {
    auto copy_src = [](uint32_t w) -> int {
        if ((w & 0x7F800000u) == 0x12000000u) return (int)bits(w, 5, 5);  // AND (imm)
        if ((w & 0x7FE0FFE0u) == 0x2A0003E0u) return (int)bits(w, 16, 5);  // MOV (ORR with zr)
        if ((w & 0x7F800000u) == 0x53000000u || (w & 0x7F800000u) == 0x13000000u)
            return (int)bits(w, 5, 5);  // UBFM/SBFM
        return -1;
    };
    std::set<int> regs = {idx};
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint64_t a = start; a < at; a += 4) {
            uint32_t w = word(a);
            int rd = (int)(w & 31), src = copy_src(w);
            if (src < 0 || src >= 31 || rd >= 31) continue;
            for (int x : {rd, src}) {
                int y = (x == rd) ? src : rd;
                if (regs.count(x) && !regs.count(y)) {
                    regs.insert(y);
                    changed = true;
                }
            }
        }
    }
    return std::vector<int>(regs.begin(), regs.end());
}

void Gen::_set_index(const std::vector<int> &regs, uint64_t i) {
    for (int r : regs) mu_->x[r] = i;
}

std::vector<uint64_t> Gen::_emulate_table(uint64_t br_pc, uint64_t entry, uint64_t end, uint64_t start,
                                          int idx, int count, uint64_t set_at) {
    Interp *mu = emu();
    int rn = (int)bits(word(br_pc), 5, 5);
    auto step_to = [&](uint64_t frm, uint64_t to) -> bool {
        uint64_t pc = frm;
        for (int k = 0; k < 64; k++) {
            if (pc == to) return true;
            if (!mu->step(pc)) return false;
        }
        return false;
    };
    std::set<uint64_t> targets;
    for (int i = 0; i < count; i++) {
        for (int r = 0; r < 29; r++) mu->x[r] = SCRATCH_MEM + 0x80000;
        mu->sp = SCRATCH_MEM + 0x80000;
        mu->nzcv = 0;
        uint64_t a2 = entry;
        while (a2 < start) {
            uint32_t w2 = word(a2);
            if ((w2 & 0x1F000000u) == 0x10000000u || (w2 & 0xFF800000u) == 0x91000000u) {
                uint64_t pc = a2;
                if (!mu->step(pc)) return {};
            }
            a2 += 4;
        }
        if (set_at != start && !step_to(start, set_at)) return {};
        _set_index({idx}, (uint64_t)i);
        if (!step_to(set_at, br_pc)) return {};
        uint64_t t = mu->x[rn];
        if (!(entry <= t && t < end) || t % 4) return {};
        targets.insert(t);
    }
    return std::vector<uint64_t>(targets.begin(), targets.end());
}

std::vector<uint64_t> Gen::_jump_table_fallthrough(uint64_t br_pc, uint64_t entry, uint64_t end) {
    int rn = (int)bits(word(br_pc), 5, 5);
    (void)rn;
    uint64_t a = br_pc - 4;
    uint64_t guard = 0, cmp = 0;
    while (a >= entry && br_pc - a <= 4 * 16) {
        uint32_t w = word(a);
        if ((w & 0xFF000010u) == 0x54000000u && (bits(w, 0, 4) == 8 || bits(w, 0, 4) == 2)) {
            guard = a;
            break;
        }
        Insn ins = lift(a);
        if (ins.kind == Insn::B || ins.kind == Insn::Bl || ins.kind == Insn::Blr ||
            ins.kind == Insn::Br || ins.kind == Insn::Ret || ins.kind == Insn::Trap)
            return {};
        a -= 4;
    }
    if (!guard) return {};
    a = guard - 4;
    while (a >= entry && guard - a <= 4 * 8) {
        uint32_t w = word(a);
        if ((w & 0x7F80001Fu) == 0x7100001Fu) {  // CMP #imm
            cmp = a;
            break;
        }
        if (lift(a).kind != Insn::Op) return {};
        a -= 4;
    }
    if (!cmp) return {};
    uint32_t w = word(cmp);
    if (bits(w, 22, 1)) return {};
    int limit = (int)bits(w, 10, 12);
    int count = bits(word(guard), 0, 4) == 8 ? limit + 1 : limit;
    if (count <= 0 || count > 4096) return {};
    int idx = (int)bits(w, 5, 5);
    uint64_t start = cmp;
    while (start - 4 >= entry && cmp - (start - 4) <= 4 * 12) {
        Insn ins = lift(start - 4);
        if (ins.kind != Insn::Op && ins.kind != Insn::Asm) break;
        start -= 4;
    }
    std::vector<int> idx_regs = _index_regs(start, cmp, idx);
    Interp *mu = emu();
    auto step_to = [&](uint64_t frm, uint64_t to) -> bool {
        uint64_t pc = frm;
        for (int k = 0; k < 64; k++) {
            if (pc == to) return true;
            if (!mu->step(pc)) return false;
        }
        return false;
    };
    std::set<uint64_t> targets;
    for (int i = 0; i < count; i++) {
        for (int r = 0; r < 29; r++) mu->x[r] = SCRATCH_MEM + 0x80000;
        mu->sp = SCRATCH_MEM + 0x80000;
        mu->nzcv = 0;
        uint64_t a2 = entry;
        while (a2 < start) {
            uint32_t w2 = word(a2);
            if ((w2 & 0x1F000000u) == 0x10000000u || (w2 & 0xFF800000u) == 0x91000000u) {
                uint64_t pc = a2;
                if (!mu->step(pc)) return {};
            }
            a2 += 4;
        }
        if (!step_to(start, cmp)) return {};
        _set_index(idx_regs, (uint64_t)i);
        if (!step_to(cmp, br_pc)) return {};
        uint64_t t = mu->x[rn];
        if (!(entry <= t && t < end) || t % 4) return {};
        targets.insert(t);
    }
    return std::vector<uint64_t>(targets.begin(), targets.end());
}

std::vector<uint64_t> Gen::_jump_table_taken(uint64_t br_pc, uint64_t entry, uint64_t end) {
    uint64_t s = br_pc;
    while (s - 4 >= entry && br_pc - (s - 4) <= 4 * 12) {
        Insn ins = lift(s - 4);
        if (ins.kind != Insn::Op && ins.kind != Insn::Asm) break;
        s -= 4;
    }
    for (uint64_t g = entry; g < end; g += 4) {
        uint32_t w = word(g);
        if ((w & 0xFF000010u) != 0x54000000u || (bits(w, 0, 4) != 9 && bits(w, 0, 4) != 3)) continue;
        uint64_t t = g + (uint64_t)sext(bits(w, 5, 19), 19) * 4;
        if (t != s) continue;
        uint32_t cw = g - 4 >= entry ? word(g - 4) : 0;
        if ((cw & 0x7F80001Fu) != 0x7100001Fu || bits(cw, 22, 1)) continue;
        int limit = (int)bits(cw, 10, 12);
        int count = bits(w, 0, 4) == 9 ? limit + 1 : limit;
        if (count <= 0 || count > 4096) continue;
        return _emulate_table(br_pc, entry, end, s, (int)bits(cw, 5, 5), count, s);
    }
    return {};
}

bool Gen::computed_target(uint64_t br_pc, uint64_t entry) {
    int rn = (int)bits(word(br_pc), 5, 5);
    uint64_t a = br_pc - 4;
    while (a >= entry && br_pc - a <= 4 * 16) {
        uint32_t w = word(a);
        Insn ins = lift(a);
        if (ins.kind != Insn::Op && ins.kind != Insn::Asm) return false;
        if ((int)(w & 31) == rn) {
            if ((w & 0x1F000000u) == 0x0B000000u) return true;  // ADD/SUB (shifted or extended reg)
            if ((w & 0x9F000000u) == 0x10000000u) return true;  // ADR
            return false;
        }
        a -= 4;
    }
    return false;
}

ScanResult Gen::scan(uint64_t entry, uint64_t end) {
    ScanResult r;
    r.labels.insert(entry);
    bool terminated = false;
    for (uint64_t a = entry; a < end; a += 4) {
        Insn ins = lift(a);
        if (ins.kind == Insn::B || ins.kind == Insn::Bcond) {
            uint64_t t = ins.target;
            if (entry <= t && t < end) r.labels.insert(t);
            else if (in_text(t)) r.ext.insert(t);
        } else if (ins.kind == Insn::Bl) {
            if (in_text(ins.target)) r.ext.insert(ins.target);
        } else if (ins.kind == Insn::Br) {
            std::vector<uint64_t> jt = jump_table(a, entry, end);
            if (!jt.empty()) {
                r.tables[a] = jt;
                for (uint64_t t : jt) r.labels.insert(t);
                stats["jumptables"]++;
            } else if (computed_target(a, entry)) {
                r.tables[a] = {};  // 'all'
                stats["br_all_labels"]++;
            } else {
                stats["br_dispatch"]++;
            }
        }
        terminated = ins.kind == Insn::B || ins.kind == Insn::Br || ins.kind == Insn::Ret ||
                     ins.kind == Insn::Trap;
    }
    if (!terminated && end < hi) r.ext.insert(end);
    return r;
}

// ---- emission

std::string Gen::render_single(const Insn &ins, uint64_t a) {
    switch (ins.kind) {
        case Insn::Asm: return Lifter::emit_asm_batch({ins.asm_insn});
        case Insn::Op:
        case Insn::Trap: return ins.code;
        case Insn::B: return "goto L_" + hex(ins.target) + ";";
        case Insn::Bcond: return "if (" + ins.code + ") { goto L_" + hex(ins.target) + "; }";
        case Insn::Bl: {
            uint64_t t = ins.target;
            if (in_text(t)) return "c->x[30] = " + hx(a + 4) + "; " + fname(t) + "(c);";
            return "c->x[30] = " + hx(a + 4) + "; c->pc = " + hx(t) + "; rt_dispatch(c);";
        }
        case Insn::Blr:
            return "c->pc = " + ins.target_expr + "; c->x[30] = " + hx(a + 4) + "; rt_dispatch(c);";
        case Insn::Br:
            return "c->pc = " + ins.target_expr + "; __attribute__((musttail)) return rt_dispatch(c);";
        case Insn::Ret: return "return;";
    }
    throw Unsupported("render_single");
}

std::vector<std::string> Gen::emit_function(uint64_t entry, uint64_t end,
                                            const std::set<uint64_t> &labels_in,
                                            const std::map<uint64_t, std::vector<uint64_t>> &tables,
                                            const std::map<uint64_t, std::string> &hle,
                                            std::set<uint64_t> &refs) {
    std::vector<std::string> out;
    out.push_back("void " + fname(entry) + "(Cpu *c) {");
    auto hit = hle.find(entry);
    if (hit != hle.end()) {
        out.push_back("    " + sanitize(hit->second) + "(c);");
        out.push_back("}");
        return out;
    }
    out.push_back("    FN_ENTER(" + hx(entry) + ");");
    std::vector<AsmInsn> batch;
    auto flush = [&]() {
        if (!batch.empty()) {
            out.push_back("    " + Lifter::emit_asm_batch(batch));
            batch.clear();
        }
    };
    auto jump = [&](uint64_t t) -> std::string {
        if (entry <= t && t < end) return "goto L_" + hex(t) + ";";
        if (!in_text(t)) return "c->pc = " + hx(t) + "; __attribute__((musttail)) return rt_dispatch(c);";
        refs.insert(t);
        return "__attribute__((musttail)) return " + fname(t) + "(c);";
    };
    bool all_labels = !tables.empty();
    std::set<uint64_t> labels = labels_in;
    if (all_labels) {
        labels.clear();
        for (uint64_t a = entry; a < end; a += 4) labels.insert(a);
    }
    std::string last;
    for (uint64_t a = entry; a < end; a += 4) {
        Insn ins = lift(a);
        if (labels.count(a)) {
            flush();
            out.push_back("L_" + hex(a) + ":;");
        }
        auto pit = patches.find(a);
        if (pit != patches.end()) {
            flush();
            out.push_back("    " + pit->second(*this, a));
            last = "op";
            continue;
        }
        switch (ins.kind) {
            case Insn::Asm: {
                if ((int)batch.size() >= 24) flush();
                batch.push_back(ins.asm_insn);
                try {
                    Lifter::emit_asm_batch(batch);
                } catch (const Unsupported &) {
                    batch.pop_back();
                    flush();
                    batch.push_back(ins.asm_insn);
                }
                last = "asm";
                break;
            }
            case Insn::Op:
            case Insn::Trap:
                flush();
                out.push_back("    " + ins.code);
                last = "op";
                break;
            case Insn::B:
                flush();
                out.push_back("    " + jump(ins.target));
                last = "b";
                break;
            case Insn::Bcond:
                flush();
                out.push_back("    if (" + ins.code + ") { " + jump(ins.target) + " }");
                last = "bcond";
                break;
            case Insn::Bl: {
                flush();
                uint64_t t = ins.target;
                if (in_text(t)) {
                    refs.insert(t);
                    out.push_back("    c->x[30] = " + hx(a + 4) + "; " + fname(t) + "(c);");
                } else {
                    out.push_back("    c->x[30] = " + hx(a + 4) + "; c->pc = " + hx(t) + "; rt_dispatch(c);");
                }
                last = "bl";
                break;
            }
            case Insn::Blr:
                flush();
                out.push_back("    c->pc = " + ins.target_expr + "; c->x[30] = " + hx(a + 4) +
                              "; rt_dispatch(c);");
                last = "blr";
                break;
            case Insn::Br: {
                flush();
                auto jt = tables.find(a);
                bool is_all = jt != tables.end() && jt->second.empty();
                if (is_all) {
                    out.push_back("    c->pc = " + ins.target_expr + "; goto L_dispatch;");
                    last = "br";
                    break;
                }
                if (jt != tables.end()) {
                    std::string cases;
                    for (size_t k = 0; k < jt->second.size(); k++)
                        cases += (k ? " " : "") + std::string("case ") + hx(jt->second[k]) + ": goto L_" +
                                 hex(jt->second[k]) + ";";
                    out.push_back("    switch (" + ins.target_expr + ") { " + cases + " default: break; }");
                    out.push_back("    c->pc = " + ins.target_expr + "; goto L_dispatch;");
                    last = "br";
                    break;
                }
                out.push_back("    c->pc = " + ins.target_expr +
                              "; __attribute__((musttail)) return rt_dispatch(c);");
                last = "br";
                break;
            }
            case Insn::Ret:
                flush();
                out.push_back("    return;");
                last = "ret";
                break;
        }
    }
    flush();
    if (last != "b" && last != "br" && last != "ret" && last != "trap") {
        if (end < hi) out.push_back("    " + jump(end));
        else out.push_back("    rt_udf(c, " + hx(end) + ");");
    }
    if (all_labels) {
        out.push_back("L_dispatch:");
        std::string cases;
        bool first = true;
        for (uint64_t t = entry; t < end; t += 4) {
            cases += (first ? "" : " ") + std::string("case ") + hx(t) + ": goto L_" + hex(t) + ";";
            first = false;
        }
        out.push_back("    switch (c->pc) { " + cases + " default: break; }");
        out.push_back("    __attribute__((musttail)) return rt_dispatch(c);");
    }
    out.push_back("}");
    return out;
}

// ------------------------------------------------------------------ write_if_changed

static void write_if_changed(const std::string &path, const std::string &text) {
    std::ifstream in(path, std::ios::binary);
    if (in) {
        std::stringstream ss;
        ss << in.rdbuf();
        if (ss.str() == text) return;
    }
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), (std::streamsize)text.size());
}

// ------------------------------------------------------------------ run_gen (gen.py main)

int run_gen(const std::string &name, const std::string &nso_path, uint64_t base,
            const std::string &outdir, const std::string &hle_path, const std::string &simd,
            int chunk, int chunk_bytes, int big_fn, int limit) {
    Gen m(name, nso_path, base, simd);
    std::vector<uint64_t> starts = m.boundaries();

    std::map<uint64_t, std::string> hle;
    if (!hle_path.empty()) {
        std::ifstream in(hle_path);
        if (in) {
            std::set<std::string> want;
            std::string line;
            while (std::getline(in, line)) {
                // strip
                size_t b = line.find_first_not_of(" \t\r\n");
                if (b == std::string::npos) continue;
                size_t e = line.find_last_not_of(" \t\r\n");
                std::string s = line.substr(b, e - b + 1);
                if (s.empty() || s[0] == '#') continue;
                want.insert(s);
            }
            for (auto &sym : m.nso.syms)
                if (sym.shndx && want.count(sym.name)) hle[base + sym.value] = sym.name;
            for (auto &kv : m.hle_extra)
                if (want.count(kv.second)) hle[kv.first] = kv.second;
        }
    }

    std::set<uint64_t> start_set(starts.begin(), starts.end());
    for (size_t i = 0; i < m.words.size(); i++) {
        uint32_t w = m.words[i];
        if ((w & 0xFC000000u) == 0x94000000u) {
            uint64_t t = m.lo + (uint64_t)i * 4 + (uint64_t)sext26(w) * 4;
            if (m.in_text(t)) start_set.insert(t);
        }
    }
    std::map<int, std::pair<uint64_t, int>> adrp_regs;  // reg -> (target, index)
    for (size_t i = 0; i < m.words.size(); i++) {
        uint32_t w = m.words[i];
        uint64_t pc = m.lo + (uint64_t)i * 4;
        if ((w & 0x9F000000u) == 0x10000000u) {  // ADR
            int64_t imm = (int64_t)(((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3));
            if (imm & (1 << 20)) imm -= 1 << 21;
            if (m.in_text(pc + (uint64_t)imm)) start_set.insert(pc + (uint64_t)imm);
        } else if ((w & 0x9F000000u) == 0x90000000u) {  // ADRP
            int64_t imm = (int64_t)(((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3));
            if (imm & (1 << 20)) imm -= 1 << 21;
            adrp_regs[(int)(w & 31)] = {(pc & ~0xFFFull) + (uint64_t)(imm << 12), (int)i};
        } else if ((w & 0xFFC00000u) == 0x91000000u) {  // ADD (imm, 64-bit, no shift)
            int rn = (int)((w >> 5) & 31), rd = (int)(w & 31);
            auto it = adrp_regs.find(rn);
            if (it != adrp_regs.end() && (int)i - it->second.second <= 8) {
                uint64_t t = it->second.first + ((w >> 10) & 0xFFF);
                if (m.in_text(t)) start_set.insert(t);
            }
            (void)rd;
        }
    }

    std::map<std::pair<uint64_t, uint64_t>, ScanResult> scanned;
    int rounds = 0;
    std::vector<uint64_t> order;
    while (true) {
        rounds++;
        order.assign(start_set.begin(), start_set.end());
        std::set<uint64_t> new_starts;
        for (size_t i = 0; i < order.size(); i++) {
            uint64_t a = order[i];
            uint64_t end = i + 1 < order.size() ? order[i + 1] : m.hi;
            auto key = std::make_pair(a, end);
            auto it = scanned.find(key);
            if (it == scanned.end()) it = scanned.emplace(key, m.scan(a, end)).first;
            for (uint64_t t : it->second.ext)
                if (!start_set.count(t)) new_starts.insert(t);
        }
        if (new_starts.empty()) break;
        start_set.insert(new_starts.begin(), new_starts.end());
    }

    struct Entry {
        uint64_t end;
        std::set<uint64_t> labels;
        std::map<uint64_t, std::vector<uint64_t>> tables;
    };
    std::map<uint64_t, Entry> entries;
    for (size_t i = 0; i < order.size(); i++) {
        uint64_t a = order[i];
        uint64_t end = i + 1 < order.size() ? order[i + 1] : m.hi;
        auto &sr = scanned[std::make_pair(a, end)];
        entries[a] = {end, sr.labels, sr.tables};
    }

    fprintf(stderr, "%s: %zu boundaries, %zu functions, %d rounds\n", name.c_str(), starts.size(),
            entries.size(), rounds);

    mkdir(outdir.c_str(), 0755);
    std::vector<uint64_t> fns_order;
    for (auto &kv : entries) fns_order.push_back(kv.first);
    if (limit > 0 && (size_t)limit < fns_order.size()) fns_order.resize((size_t)limit);

    int nchunks = 0;
    size_t ci = 0;
    struct Pending {
        uint64_t a;
        std::vector<std::string> lines;
        std::set<uint64_t> refs;
    };
    Pending *pending = nullptr;
    while (ci < fns_order.size() || pending) {
        std::vector<std::string> body;
        std::set<uint64_t> refs;
        int size = 0;
        std::vector<uint64_t> fns;
        while ((pending || ci < fns_order.size()) && (int)fns.size() < chunk && size < chunk_bytes) {
            uint64_t a;
            std::vector<std::string> lines;
            std::set<uint64_t> r;
            if (pending) {
                a = pending->a;
                lines = pending->lines;
                r = pending->refs;
                delete pending;
                pending = nullptr;
            } else {
                a = fns_order[ci++];
                Entry &e = entries[a];
                lines = m.emit_function(a, e.end, e.labels, e.tables, hle, r);
            }
            int n = 0;
            for (auto &l : lines) n += (int)l.size() + 1;
            if (n > big_fn && !fns.empty()) {
                pending = new Pending{a, lines, r};
                break;
            }
            for (auto &l : lines) body.push_back(l);
            size += n;
            fns.push_back(a);
            refs.insert(r.begin(), r.end());
            if (n > big_fn) break;
        }
        char path[1024];
        snprintf(path, sizeof path, "%s/%s_%03d.c", outdir.c_str(), name.c_str(), nchunks);
        std::string text;
        text += "#include \"cpu.h\"\n";
        if (m.lifter.sc) text += "#include \"simd.h\"\n";
        for (uint64_t r : refs) text += "void " + fname(r) + "(Cpu *c);\n";
        std::set<uint64_t> hle_in_chunk;
        for (uint64_t a : fns)
            if (hle.count(a)) hle_in_chunk.insert(a);
        for (uint64_t a : hle_in_chunk) text += "void " + sanitize(hle[a]) + "(Cpu *c);\n";
        for (size_t k = 0; k < body.size(); k++) text += body[k] + "\n";
        text += "\n";
        write_if_changed(path, text);
        nchunks++;
        fprintf(stderr, "%s: progress %zu/%zu files %d\n", name.c_str(), ci, fns_order.size(), nchunks);
    }

    // table.c
    {
        std::string t;
        t += "#include \"cpu.h\"\n";
        for (uint64_t a : fns_order) t += "void " + fname(a) + "(Cpu *c);\n";
        t += "const uint64_t " + name + "_fn_addrs[] = {\n";
        for (size_t k = 0; k < fns_order.size(); k++) t += (k ? ",\n" : "") + hx(fns_order[k]);
        t += "\n};\n";
        t += "const GuestFn " + name + "_fn_ptrs[] = {\n";
        for (size_t k = 0; k < fns_order.size(); k++) t += (k ? ",\n" : "") + fname(fns_order[k]);
        t += "\n};\n";
        t += "const uint64_t " + name + "_fn_count = " + std::to_string(fns_order.size()) + ";\n";
        write_if_changed(outdir + "/" + name + "_table.c", t);
    }
    fprintf(stderr, "%s: wrote %d chunks\n", name.c_str(), nchunks);
    return 0;
}

}  // namespace ngen
