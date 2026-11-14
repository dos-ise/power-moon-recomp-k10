#include "simd_c.h"

#include <map>
#include <sstream>
#include <vector>

#include "arm64.h"
#include "lifter.h"

namespace ngen {

namespace {

// ---- string helpers (must match simd_c.py exactly)

std::string lop(const std::string &name, int u = 0) {
    return "LOP_" + name + (u ? " | LOP_U" : "");
}
std::string vu(int sz) {
    static const char *t[4] = {"vu8", "vu16", "vu32", "vu64"};
    return t[sz];
}
std::string vs(int sz) {
    static const char *t[4] = {"vs8", "vs16", "vs32", "vs64"};
    return t[sz];
}
std::string ut(int sz) {
    static const char *t[4] = {"uint8_t", "uint16_t", "uint32_t", "uint64_t"};
    return t[sz];
}
std::string VR(const std::string &T, int n) { return "VREG(" + T + ", " + std::to_string(n) + ")"; }
std::string VQ(const std::string &T, int Q, int n) {
    return Q ? VR(T, n) : "VQ(" + T + ", 0, " + std::to_string(n) + ")";
}
std::string VWr(const std::string &T, int Q, int d, const std::string &e) {
    return "VW(" + T + ", " + std::to_string(Q) + ", " + std::to_string(d) + ", " + e + ");";
}
std::string cond(int c) {
    if (c < 14) {
        static const char *names[16] = {"EQ", "NE", "CS", "CC", "MI", "PL", "VS", "VC",
                                        "HI", "LS", "GE", "LT", "GT", "LE", "AL", "NV"};
        return "COND_" + std::string(names[c]) + "(c)";
    }
    return "1";
}
std::string up(const std::string &s) {
    std::string r = s;
    for (auto &c : r) c = (char)toupper(c);
    return r;
}
std::string gen(const std::string &op, int sz, int ne, int d, int szd, int offd, int keep, int n,
                int szn, int offn, int m = -1, int szm = 0, int offm = 0, int bidx = -1,
                uint64_t bimm = 0) {
    return "sl_gen(c, " + op + ", " + std::to_string(sz) + ", " + std::to_string(ne) + ", " +
           std::to_string(d) + ", " + std::to_string(szd) + ", " + std::to_string(offd) + ", " +
           std::to_string(keep) + ", " + std::to_string(n) + ", " + std::to_string(szn) + ", " +
           std::to_string(offn) + ", " + std::to_string(m) + ", " + std::to_string(szm) + ", " +
           std::to_string(offm) + ", " + std::to_string(bidx) + ", " + hx(bimm) + ");";
}
std::string same(const std::string &op, int sz, int ne, int d, int n, int m = -1, uint64_t bimm = 0,
                 int bidx = -1) {
    return gen(op, sz, ne, d, sz, 0, 0, n, sz, 0, m, sz, 0, bidx, bimm);
}
int lanes(int Q, int sz) { return (8 << Q) >> sz; }
std::string shuf(const std::string &T, const std::string &a, const std::string &b,
                 const std::vector<int> &idx) {
    std::string s;
    for (size_t k = 0; k < idx.size(); k++) s += (k ? ", " : "") + std::to_string(idx[k]);
    return "SHUF(" + a + ", " + b + ", " + s + ")";
}
std::string u128(__uint128_t v) {
    uint64_t lo = (uint64_t)v, hi = (uint64_t)(v >> 64);
    if (hi) return "((u128)" + hx(hi) + " << 64 | " + hx(lo) + ")";
    return hx(lo);
}

struct FT_t {
    const char *F, *VT, *VU, *T, *SF, *SU, *SET;
};
const FT_t FT[2] = {{"s", "vf32", "vu32", "float", "VSF32", "VS32", "FP_SETS"},
                    {"d", "vf64", "vu64", "double", "VSF64", "VS64", "FP_SETD"}};

// FOP3: (U, a_, opc) -> FOP
const std::map<std::tuple<int, int, int>, const char *> FOP3 = {
    {{0, 0, 0b11000}, "FOP_MAXNM"}, {{0, 0, 0b11010}, "FOP_ADD"}, {{0, 0, 0b11011}, "FOP_MULX"},
    {{0, 0, 0b11110}, "FOP_MAX"}, {{0, 0, 0b11111}, "FOP_RECPS"},
    {{0, 1, 0b11000}, "FOP_MINNM"}, {{0, 1, 0b11010}, "FOP_SUB"}, {{0, 1, 0b11110}, "FOP_MIN"},
    {{0, 1, 0b11111}, "FOP_RSQRTS"},
    {{1, 0, 0b11011}, "FOP_MUL"}, {{1, 0, 0b11111}, "FOP_DIV"}, {{1, 1, 0b11010}, "FOP_ABD"},
};
const std::map<std::tuple<int, int, int>, const char *> FPAIRV = {
    {{1, 0, 0b11000}, "FOP_MAXNM"}, {{1, 1, 0b11000}, "FOP_MINNM"}, {{1, 0, 0b11010}, "FOP_ADD"},
    {{1, 0, 0b11110}, "FOP_MAX"}, {{1, 1, 0b11110}, "FOP_MIN"},
};
const std::map<std::tuple<int, int, int>, int> FCMP3 = {
    {{0, 0, 0b11100}, 0}, {{1, 0, 0b11100}, 1}, {{1, 1, 0b11100}, 2},
    {{1, 0, 0b11101}, 3}, {{1, 1, 0b11101}, 4},
};

uint64_t vfp_expand_imm(int imm8, int N) {
    int E = N == 32 ? 8 : 11;
    int F = N - E - 1;
    int sign = imm8 >> 7;
    int b6 = (imm8 >> 6) & 1;
    int exp = ((b6 ^ 1) << (E - 1)) | ((((1 << (E - 3)) - 1) * (b6 ? 1 : 0)) << 2) | ((imm8 >> 4) & 3);
    uint64_t frac = (uint64_t)(imm8 & 15) << (F - 4);
    return ((uint64_t)sign << (N - 1)) | ((uint64_t)exp << F) | frac;
}

uint64_t simd_expand_imm(int op, int cmode, int imm8) {
    int c = cmode >> 1;
    uint64_t imm = 0;
    if (c == 0) imm = ((uint64_t)imm8 << 32) | (uint64_t)imm8;
    else if (c == 1) imm = (((uint64_t)imm8 << 8) << 32) | ((uint64_t)imm8 << 8);
    else if (c == 2) imm = (((uint64_t)imm8 << 16) << 32) | ((uint64_t)imm8 << 16);
    else if (c == 3) imm = (((uint64_t)imm8 << 24) << 32) | ((uint64_t)imm8 << 24);
    else if (c == 4) {
        for (int s = 0; s < 64; s += 16) imm |= (uint64_t)imm8 << s;
    } else if (c == 5) {
        for (int s = 0; s < 64; s += 16) imm |= (uint64_t)(imm8 << 8) << s;
    } else if (c == 6) {
        uint64_t v = (cmode & 1) ? ((uint64_t)(imm8 << 16) | 0xFFFF) : ((uint64_t)(imm8 << 8) | 0xFF);
        imm = (v << 32) | v;
    } else {
        if (!(cmode & 1) && !op) {
            for (int s = 0; s < 64; s += 8) imm |= (uint64_t)imm8 << s;
        } else if (!(cmode & 1) && op) {
            for (int i = 0; i < 8; i++)
                if ((imm8 >> i) & 1) imm |= 0xFFull << (8 * i);
        } else if ((cmode & 1) && !op) {
            uint64_t v = vfp_expand_imm(imm8, 32);
            imm = (v << 32) | v;
        } else {
            imm = vfp_expand_imm(imm8, 64);
        }
    }
    return imm;
}

}  // namespace

// ------------------------------------------------------------ top level

std::string SimdC::dp(uint32_t w, uint64_t pc) {
    (void)pc;
    int op0 = (int)bits(w, 28, 4), op1 = (int)bits(w, 23, 2), op2 = (int)bits(w, 19, 4),
        op3 = (int)bits(w, 10, 9);
    if (op0 == 0b0100 && !(op1 & 2) && (op2 & 7) == 5 && (op3 & 0b110000011) == 0b10)
        return crypto_aes(w);
    if (op0 == 0b0101 && !(op1 & 2) && !(op2 & 4) && (op3 & 0b000100011) == 0) return crypto_sha3(w);
    if (op0 == 0b0101 && !(op1 & 2) && (op2 & 7) == 5 && (op3 & 0b110000011) == 0b10)
        return crypto_sha2(w);
    if ((op0 & 0b1101) == 0b0101) {  // AdvSIMD scalar
        if (op1 == 0 && !(op2 & 0b1100) && (op3 & 0b000100001) == 1) return scalar_copy(w);
        if (!(op1 & 2) && (op2 & 7) == 4 && (op3 & 0b110000011) == 0b10) return misc2(w, true);
        if (!(op1 & 2) && (op2 & 7) == 6 && (op3 & 0b110000011) == 0b10) return scalar_pairwise(w);
        if (!(op1 & 2) && (op2 & 4) && (op3 & 3) == 0) return three_diff(w, true);
        if (!(op1 & 2) && (op2 & 4) && (op3 & 1)) return three_same(w, true);
        if (op1 == 2 && (op3 & 1)) return shift_imm(w, true);
        if ((op1 & 2) && !(op3 & 1)) return indexed(w, true);
        throw Unsupported("simd scalar");
    }
    if ((op0 & 0b1011) == 0 && !(op1 & 2) && !(op2 & 4) && (op3 & 0b000100011) == 0) return tbl(w);
    if ((op0 & 0b1011) == 0 && !(op1 & 2) && !(op2 & 4) && (op3 & 0b000100011) == 2) return permute(w);
    if ((op0 & 0b1011) == 0b0010 && !(op1 & 2) && !(op2 & 4) && (op3 & 0b000100001) == 0) return ext(w);
    if ((op0 & 0b1001) == 0) {  // AdvSIMD vector
        if (op1 == 0 && !(op2 & 0b1100) && (op3 & 0b000100001) == 1) return copy(w);
        if (!(op1 & 2) && (op2 & 4) && (op3 & 1)) return three_same(w, false);
        if (!(op1 & 2) && (op2 & 4) && (op3 & 3) == 0) return three_diff(w, false);
        if (!(op1 & 2) && (op2 & 7) == 4 && (op3 & 0b110000011) == 0b10) return misc2(w, false);
        if (!(op1 & 2) && (op2 & 7) == 6 && (op3 & 0b110000011) == 0b10) return across(w);
        if (op1 == 2 && op2 == 0 && (op3 & 1)) return modimm(w);
        if (op1 == 2 && op2 && (op3 & 1)) return shift_imm(w, false);
        if ((op1 & 2) && !(op3 & 1)) return indexed(w, false);
        throw Unsupported("simd vector");
    }
    if ((op0 & 0b0101) == 0b0001) {  // scalar FP
        if (bits(w, 29, 1) || (bits(w, 31, 1) && (op1 & 2 || (op2 & 4 && op3 & 0b111111))))
            throw Unsupported("fp M/S");
        if (op1 & 2) return fp_dp3(w);
        if (!(op2 & 4)) return fp_fixed(w);
        if ((op3 & 0b111111) == 0) return fp_int(w);
        if ((op3 & 0b11111) == 0b10000) return fp_dp1(w);
        if ((op3 & 0b1111) == 0b1000) return fp_cmp(w);
        if ((op3 & 0b111) == 0b100) return fp_imm(w);
        if ((op3 & 3) == 1) return fp_ccmp(w);
        if ((op3 & 3) == 2) return fp_dp2(w);
        if ((op3 & 3) == 3) return fp_csel(w);
        throw Unsupported("fp op3");
    }
    throw Unsupported("fp/simd");
}

// ------------------------------------------------------------ crypto

std::string SimdC::crypto_aes(uint32_t w) {
    if (bits(w, 22, 2)) throw Unsupported("aes size");
    int opc = (int)bits(w, 12, 5);
    if (opc < 4 || opc > 7) throw Unsupported("aes opcode");
    return "sv_aes(c, " + std::to_string(opc - 4) + ", " + std::to_string(bits(w, 0, 5)) + ", " +
           std::to_string(bits(w, 5, 5)) + ");";
}

std::string SimdC::crypto_sha3(uint32_t w) {
    if (bits(w, 22, 2)) throw Unsupported("sha size");
    int opc = (int)bits(w, 12, 3), d = (int)bits(w, 0, 5), n = (int)bits(w, 5, 5),
        m = (int)bits(w, 16, 5);
    if (opc < 3) return "sv_sha1hash(c, " + std::to_string(opc) + ", " + std::to_string(d) + ", " +
                        std::to_string(n) + ", " + std::to_string(m) + ");";
    if (opc == 3) return "sv_sha_misc(c, 0, " + std::to_string(d) + ", " + std::to_string(n) + ", " +
                         std::to_string(m) + ");";
    if (opc == 4 || opc == 5)
        return "sv_sha256hash(c, " + std::to_string(opc == 4) + ", " + std::to_string(d) + ", " +
               std::to_string(n) + ", " + std::to_string(m) + ");";
    if (opc == 6) return "sv_sha_misc(c, 3, " + std::to_string(d) + ", " + std::to_string(n) + ", " +
                         std::to_string(m) + ");";
    throw Unsupported("sha3 opcode");
}

std::string SimdC::crypto_sha2(uint32_t w) {
    if (bits(w, 22, 2)) throw Unsupported("sha size");
    int opc = (int)bits(w, 12, 5), d = (int)bits(w, 0, 5), n = (int)bits(w, 5, 5);
    int op;
    if (opc == 0) op = 4;
    else if (opc == 1) op = 1;
    else if (opc == 2) op = 2;
    else throw Unsupported("sha2 opcode");
    return "sv_sha_misc(c, " + std::to_string(op) + ", " + std::to_string(d) + ", " + std::to_string(n) +
           ", 0);";
}

// ------------------------------------------------------------ copy / permute / table

static int imm5_sz(int imm5, int &idx) {
    if (!(imm5 & 15)) throw Unsupported("copy imm5");
    int sz = __builtin_ctz(imm5);
    idx = imm5 >> (sz + 1);
    return sz;
}

std::string SimdC::scalar_copy(uint32_t w) {
    if (bits(w, 29, 1) || bits(w, 11, 4)) throw Unsupported("scalar copy");
    int idx;
    int sz = imm5_sz((int)bits(w, 16, 5), idx);
    if (sz > 3) throw Unsupported("scalar dup size");
    int d = (int)bits(w, 0, 5), n = (int)bits(w, 5, 5);
    static const char *P[4] = {"uint8_t", "sa16", "sa32", "sa64"};
    return "c->v[" + std::to_string(d) + "] = ((" + P[sz] + " *)&c->v[" + std::to_string(n) + "])[" +
           std::to_string(idx) + "];";
}

std::string SimdC::copy(uint32_t w) {
    int Q = (int)bits(w, 30, 1), op = (int)bits(w, 29, 1), imm4 = (int)bits(w, 11, 4);
    int d = (int)bits(w, 0, 5), n = (int)bits(w, 5, 5);
    int idx;
    int sz = imm5_sz((int)bits(w, 16, 5), idx);
    if (sz > 3) throw Unsupported("copy size");
    static const char *P[4] = {"uint8_t", "sa16", "sa32", "sa64"};
    if (op) {
        if (!Q) throw Unsupported("ins elem Q");
        int idx2 = imm4 >> sz;
        return "((" + std::string(P[sz]) + " *)&c->v[" + std::to_string(d) + "])[" + std::to_string(idx) +
               "] = ((" + P[sz] + " *)&c->v[" + std::to_string(n) + "])[" + std::to_string(idx2) + "];";
    }
    if (imm4 == 0) {  // DUP (element)
        if (sz == 3 && !Q) throw Unsupported("dup 1d");
        int nl = 16 >> sz;
        std::vector<int> idxs(nl, idx);
        return VWr(vu(sz), Q, d, shuf(vu(sz), VR(vu(sz), n), VR(vu(sz), n), idxs));
    }
    if (imm4 == 1) {  // DUP (general)
        if (sz == 3 && !Q) throw Unsupported("dup 1d");
        return VWr(vu(sz), Q, d, "(" + vu(sz) + "){} + (" + ut(sz) + ")" + X(n));
    }
    if (imm4 == 3) {  // INS (general)
        if (!Q) throw Unsupported("ins Q");
        return "((" + std::string(P[sz]) + " *)&c->v[" + std::to_string(d) + "])[" + std::to_string(idx) +
               "] = (" + ut(sz) + ")" + X(n) + ";";
    }
    if (imm4 == 5) {  // SMOV
        if (sz >= 2 + Q) throw Unsupported("smov size");
        static const char *st[3] = {"int8_t", "int16_t", "int32_t"};
        return set_reg(Q, d, "(uint64_t)(int64_t)(" + std::string(st[sz]) + ")((" + P[sz] + " *)&c->v[" +
                                  std::to_string(n) + "])[" + std::to_string(idx) + "]");
    }
    if (imm4 == 7) {  // UMOV
        if ((Q && sz != 3) || (!Q && sz == 3)) throw Unsupported("umov size");
        return set_reg(Q, d, "((" + std::string(P[sz]) + " *)&c->v[" + std::to_string(n) + "])[" +
                                  std::to_string(idx) + "]");
    }
    throw Unsupported("copy imm4");
}

std::string SimdC::permute(uint32_t w) {
    int Q = (int)bits(w, 30, 1), sz = (int)bits(w, 22, 2), m = (int)bits(w, 16, 5),
        opc = (int)bits(w, 12, 3), n = (int)bits(w, 5, 5), d = (int)bits(w, 0, 5);
    if (opc == 0 || opc == 4 || (sz == 3 && !Q)) throw Unsupported("permute");
    int ne = lanes(Q, sz), NL = 16 >> sz;
    int part = opc >> 2, kind = opc & 3;
    std::vector<int> idx;
    for (int i = 0; i < ne; i++) {
        if (kind == 1) {  // UZP
            int j = 2 * i + part;
            idx.push_back(j < ne ? j : NL + j - ne);
        } else if (kind == 2) {  // TRN
            int p = i & ~1;
            idx.push_back((i & 1) ? NL + p + part : p + part);
        } else {  // ZIP
            int base = part * ne / 2;
            idx.push_back((i & 1) ? NL + base + i / 2 : base + i / 2);
        }
    }
    while ((int)idx.size() < NL) idx.push_back(0);
    return VWr(vu(sz), Q, d, shuf(vu(sz), VR(vu(sz), n), VR(vu(sz), m), idx));
}

std::string SimdC::ext(uint32_t w) {
    int Q = (int)bits(w, 30, 1), m = (int)bits(w, 16, 5), imm4 = (int)bits(w, 11, 4),
        n = (int)bits(w, 5, 5), d = (int)bits(w, 0, 5);
    if (bits(w, 22, 2) || (!Q && imm4 >= 8)) throw Unsupported("ext");
    std::vector<int> idx;
    if (Q) {
        for (int i = 0; i < 16; i++) idx.push_back(imm4 + i);
    } else {
        for (int i = 0; i < 8; i++) idx.push_back(imm4 + i < 8 ? imm4 + i : 16 + imm4 + i - 8);
        while ((int)idx.size() < 16) idx.push_back(0);
    }
    return VWr("vu8", Q, d, shuf("vu8", VR("vu8", n), VR("vu8", m), idx));
}

std::string SimdC::tbl(uint32_t w) {
    int Q = (int)bits(w, 30, 1), m = (int)bits(w, 16, 5), ln = (int)bits(w, 13, 2),
        op = (int)bits(w, 12, 1), n = (int)bits(w, 5, 5), d = (int)bits(w, 0, 5);
    if (bits(w, 22, 2)) throw Unsupported("tbl op2");
    return "sv_tbl(c, " + std::to_string(Q) + ", " + std::to_string(ln) + ", " + std::to_string(op) +
           ", " + std::to_string(d) + ", " + std::to_string(n) + ", " + std::to_string(m) + ");";
}

// ------------------------------------------------------------ three same

static std::pair<std::vector<int>, std::vector<int>> pair_idx(int Q, int sz) {
    int ne = lanes(Q, sz), NL = 16 >> sz;
    std::vector<int> src;
    for (int i = 0; i < ne; i++) src.push_back(i);
    for (int i = 0; i < ne; i++) src.push_back(NL + i);
    std::vector<int> ev, od;
    for (int k = 0; k < ne; k++) ev.push_back(src[2 * k]);
    for (int k = 0; k < ne; k++) od.push_back(src[2 * k + 1]);
    while ((int)ev.size() < NL) ev.push_back(ne % NL);
    while ((int)od.size() < NL) od.push_back(ne % NL);
    return {ev, od};
}

std::string SimdC::three_same(uint32_t w, bool scalar) {
    int Q = (int)bits(w, 30, 1), U = (int)bits(w, 29, 1), sz = (int)bits(w, 22, 2),
        m = (int)bits(w, 16, 5), opc = (int)bits(w, 11, 5), n = (int)bits(w, 5, 5),
        d = (int)bits(w, 0, 5);
    if (opc >= 0b11000) return three_same_fp(w, scalar);
    if (scalar) return three_same_scalar(U, sz, opc, d, n, m);
    if (sz == 3 && !Q && opc != 3) throw Unsupported("3same 1d");
    int ne = lanes(Q, sz);
    std::string T = vu(sz), S = vs(sz);
    std::string a = VR(T, n), b = VR(T, m), o = VR(T, d);
    std::string sa = VR(S, n), sb = VR(S, m);
    std::string x = U ? a : sa, y = U ? b : sb;
    if ((opc == 0 || opc == 2 || opc == 4 || opc == 14 || opc == 15 || opc == 12 || opc == 13) && sz == 3)
        throw Unsupported("3same size");
    if (opc == 0) return same(lop("HADD", U), sz, ne, d, n, m);
    if (opc == 1)
        return VWr(T, Q, d, "(" + T + ")__builtin_elementwise_" + (U ? "add" : "add") + "_sat(" + x + ", " + y + ")");
    if (opc == 2) return same(lop("RHADD", U), sz, ne, d, n, m);
    if (opc == 3) {
        std::string a64 = VR("vu64", n), b64 = VR("vu64", m), o64 = VR("vu64", d);
        // U=0: AND, BIC, ORR, ORN ; U=1: EOR, BSL, BIT, BIF
        std::string expr;
        if (U == 0) {
            if (sz == 0) expr = a64 + " & " + b64;
            else if (sz == 1) expr = a64 + " & ~" + b64;
            else if (sz == 2) expr = a64 + " | " + b64;
            else expr = a64 + " | ~" + b64;
        } else {
            if (sz == 0) expr = a64 + " ^ " + b64;
            else if (sz == 1) expr = "(" + o64 + " & " + a64 + ") | (~" + o64 + " & " + b64 + ")";
            else if (sz == 2) expr = "(" + o64 + " & ~" + b64 + ") | (" + a64 + " & " + b64 + ")";
            else expr = "(" + o64 + " & " + b64 + ") | (" + a64 + " & ~" + b64 + ")";
        }
        return VWr("vu64", Q, d, expr);
    }
    if (opc == 4) return same(lop("HSUB", U), sz, ne, d, n, m);
    if (opc == 5)
        return VWr(T, Q, d, "(" + T + ")__builtin_elementwise_sub_sat(" + x + ", " + y + ")");
    if (opc == 6) return VWr(T, Q, d, "(" + T + ")(" + x + " > " + y + ")");
    if (opc == 7) return VWr(T, Q, d, "(" + T + ")(" + x + " >= " + y + ")");
    if (opc >= 8 && opc <= 11) {
        static const char *nm[4] = {"SHL", "QSHL", "RSHL", "QRSHL"};
        return same(lop(nm[opc - 8], U), sz, ne, d, n, m);
    }
    if (opc == 12) return VWr(T, Q, d, "(" + T + ")__builtin_elementwise_max(" + x + ", " + y + ")");
    if (opc == 13) return VWr(T, Q, d, "(" + T + ")__builtin_elementwise_min(" + x + ", " + y + ")");
    if (opc == 14 || opc == 15) {
        std::string e = "(" + T + ")__builtin_elementwise_max(" + x + ", " + y + ") - (" + T +
                        ")__builtin_elementwise_min(" + x + ", " + y + ")";
        return VWr(T, Q, d, opc == 15 ? o + " + (" + e + ")" : e);
    }
    if (opc == 16) return VWr(T, Q, d, a + (U ? " - " : " + ") + b);
    if (opc == 17)
        return VWr(T, Q, d, U ? "(" + T + ")(" + a + " == " + b + ")"
                              : "(" + T + ")((" + a + " & " + b + ") != 0)");
    if (sz == 3 && opc != 23) throw Unsupported("3same size");
    if (opc == 18) return VWr(T, Q, d, o + (U ? " - " : " + ") + a + " * " + b);
    if (opc == 19) {
        if (U) {
            if (sz) throw Unsupported("pmul size");
            return same(lop("PMUL"), 0, ne, d, n, m);
        }
        return VWr(T, Q, d, a + " * " + b);
    }
    if (opc == 20 || opc == 21 || opc == 23) {
        if (opc == 23 && U) throw Unsupported("addp U");
        auto [ev, od] = pair_idx(Q, sz);
        std::string A = (U || opc == 23) ? a : sa;
        std::string B = (U || opc == 23) ? b : sb;
        std::string L = shuf(T, A, B, ev), R = shuf(T, A, B, od);
        if (opc == 23) return VWr(T, Q, d, "(" + T + ")(" + L + " + " + R + ")");
        return VWr(T, Q, d, "(" + T + ")__builtin_elementwise_" + (opc == 20 ? "max" : "min") + "(" + L +
                                  ", " + R + ")");
    }
    if (opc == 22) {
        if (sz != 1 && sz != 2) throw Unsupported("sqdmulh size");
        return same(lop(U ? "SQRDMULH" : "SQDMULH"), sz, ne, d, n, m);
    }
    throw Unsupported("3same opcode");
}

std::string SimdC::three_same_scalar(int U, int sz, int opc, int d, int n, int m) {
    if (opc == 1 || opc == 5) {
        std::string nm = opc == 1 ? (U ? "UQADD" : "SQADD") : (U ? "UQSUB" : "SQSUB");
        return same(lop(nm), sz, 1, d, n, m);
    }
    if (opc == 9 || opc == 11) {
        std::string nm = opc == 9 ? "QSHL" : "QRSHL";
        return same(lop(nm, U), sz, 1, d, n, m);
    }
    if (opc == 22) {
        if (sz != 1 && sz != 2) throw Unsupported("sqdmulh size");
        return same(lop(U ? "SQRDMULH" : "SQDMULH"), sz, 1, d, n, m);
    }
    if (sz != 3) throw Unsupported("scalar 3same size");
    std::string a = "VS64(" + std::to_string(n) + ")", b = "VS64(" + std::to_string(m) + ")";
    std::string sa = "(int64_t)VS64(" + std::to_string(n) + ")", sb = "(int64_t)VS64(" + std::to_string(m) + ")";
    std::string x = U ? a : sa, y = U ? b : sb;
    if (opc == 6) return "c->v[" + std::to_string(d) + "] = (" + x + " > " + y + ") ? ~0ull : 0;";
    if (opc == 7) return "c->v[" + std::to_string(d) + "] = (" + x + " >= " + y + ") ? ~0ull : 0;";
    if (opc == 8 || opc == 10) {
        std::string nm = opc == 8 ? "SHL" : "RSHL";
        return same(lop(nm, U), 3, 1, d, n, m);
    }
    if (opc == 16) return "c->v[" + std::to_string(d) + "] = (uint64_t)(" + a + (U ? " - " : " + ") + b + ");";
    if (opc == 17)
        return U ? "c->v[" + std::to_string(d) + "] = (" + a + " == " + b + ") ? ~0ull : 0;"
                 : "c->v[" + std::to_string(d) + "] = (" + a + " & " + b + ") ? ~0ull : 0;";
    throw Unsupported("scalar 3same opcode");
}

std::string SimdC::three_same_fp(uint32_t w, bool scalar) {
    int Q = (int)bits(w, 30, 1), U = (int)bits(w, 29, 1), a_ = (int)bits(w, 23, 1),
        sz = (int)bits(w, 22, 1), m = (int)bits(w, 16, 5), opc = (int)bits(w, 11, 5),
        n = (int)bits(w, 5, 5), d = (int)bits(w, 0, 5);
    if (!scalar && sz && !Q) throw Unsupported("fp 2d Q");
    const FT_t &ft = FT[sz];
    std::string F = ft.F, VT = ft.VT, VU = ft.VU, T = ft.T, SF = ft.SF, SU = ft.SU, SET = ft.SET;
    auto key = std::make_tuple(U, a_, opc);
    if (FCMP3.count(key)) {
        int cop = FCMP3.at(key);
        if (scalar)
            return "c->v[" + std::to_string(d) + "] = (" + ut(2 + sz) + ")vcmp_" + F + "(c, " +
                   std::to_string(cop) + ", (" + VT + "){" + SF + "(" + std::to_string(n) + ")}, (" + VT +
                   "){})[0];";
        return VWr(VU, Q, d, "vcmp_" + F + "(c, " + std::to_string(cop) + ", " + VQ(VT, Q, n) + ", " +
                                  VQ(VT, Q, m) + ")");
    }
    if (scalar) {
        if (key != std::make_tuple(0, 0, 0b11011) && key != std::make_tuple(0, 0, 0b11111) &&
            key != std::make_tuple(0, 1, 0b11111) && key != std::make_tuple(1, 1, 0b11010))
            throw Unsupported("scalar fp 3same");
        const char *fop = FOP3.at(key);
        std::string x = (std::string(fop) == "FOP_RECPS" || std::string(fop) == "FOP_RSQRTS")
                            ? "neg_" + F + "(" + SF + "(" + std::to_string(n) + "))"
                            : SF + "(" + std::to_string(n) + ")";
        return SET + "(" + std::to_string(d) + ", op_" + F + "(c, " + fop + ", " + x + ", " + SF + "(" +
               std::to_string(m) + "), 0));";
    }
    std::string A = VQ(VT, Q, n), B = VQ(VT, Q, m);
    if (FPAIRV.count(key)) {
        auto [ev, od] = pair_idx(Q, 2 + sz);
        return "SV_FOP_" + up(F) + "(" +
               std::string(FPAIRV.at(key)) + ", " + std::to_string(Q) + ", " + std::to_string(d) + ", " +
               shuf(VT, A, B, ev) + ", " + shuf(VT, A, B, od) + ", (" + VT + "){});";
    }
    if (U == 0 && opc == 0b11001) {  // FMLA / FMLS
        return "SV_FOP_" + up(F) +
               "(FOP_MADD, " + std::to_string(Q) + ", " + std::to_string(d) + ", " + VQ(VT, Q, d) + ", " +
               (a_ ? "vneg_" + F + "(" + A + ")" : A) + ", " + B + ");";
    }
    if (!FOP3.count(key)) throw Unsupported("fp 3same opcode");
    const char *fop = FOP3.at(key);
    if (std::string(fop) == "FOP_RECPS" || std::string(fop) == "FOP_RSQRTS") A = "vneg_" + F + "(" + A + ")";
    return "SV_FOP_" + up(F) + "(" + fop + ", " +
           std::to_string(Q) + ", " + std::to_string(d) + ", " + A + ", " + B + ", (" + VT + "){});";
}

// ------------------------------------------------------------ three different

std::string SimdC::three_diff(uint32_t w, bool scalar) {
    int Q = (int)bits(w, 30, 1), U = (int)bits(w, 29, 1), sz = (int)bits(w, 22, 2),
        m = (int)bits(w, 16, 5), opc = (int)bits(w, 12, 4), n = (int)bits(w, 5, 5),
        d = (int)bits(w, 0, 5);
    if (scalar) {
        if (U || (opc != 9 && opc != 11 && opc != 13) || (sz != 1 && sz != 2))
            throw Unsupported("scalar 3diff");
        static const char *op[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, "SQDMLAL", 0, "SQDMLSL", 0, "SQDMULL", 0, 0};
        return gen(lop(op[opc]), sz, 1, d, sz + 1, 0, 0, n, sz, 0, m, sz, 0);
    }
    if (opc == 14) {  // PMULL
        if (U) throw Unsupported("pmull U");
        if (sz == 3) return "sl_pmull64(c, " + std::to_string(Q) + ", " + std::to_string(d) + ", " +
                            std::to_string(n) + ", " + std::to_string(m) + ");";
        if (sz) throw Unsupported("pmull size");
    }
    if (sz == 3 || opc == 15) throw Unsupported("3diff size");
    int ne = 8 >> sz;
    int off = Q * ne;
    if (opc == 4 || opc == 6) {  // ADDHN/RADDHN/SUBHN/RSUBHN
        static const char *nm[4] = {"ADDHN", "RADDHN", "SUBHN", "RSUBHN"};
        const char *op = nm[(opc == 6) * 2 + U];
        return gen(lop(op), sz, ne, d, sz, off, Q, n, sz + 1, 0, m, sz + 1, 0);
    }
    if (opc == 1 || opc == 3) {  // wide
        const char *op = opc == 1 ? "ADDW" : "SUBW";
        return gen(lop(op, U), sz, ne, d, sz + 1, 0, 0, n, sz + 1, 0, m, sz, off);
    }
    static const char *opmap[16] = {"ADDL", 0, "SUBL", 0, 0, "ABAL", 0, "ABDL", "MLAL", "SQDMLAL",
                                    "MLSL", "SQDMLSL", "MULL", "SQDMULL", "PMULL", 0};
    const char *op = opmap[opc];
    if (!op) throw Unsupported("3diff opcode");
    if (op[0] == 'S' && op[1] == 'Q' && op[2] == 'D') {
        if (U || (sz != 1 && sz != 2)) throw Unsupported("sqdmull");
    }
    int u = (op[0] == 'S' && op[1] == 'Q' && op[2] == 'D') || std::string(op) == "PMULL" ? 0 : U;
    return gen(lop(op, u), sz, ne, d, sz + 1, 0, 0, n, sz, off, m, sz, off);
}

// ------------------------------------------------------------ two-register misc

std::string SimdC::misc2(uint32_t w, bool scalar) {
    int Q = (int)bits(w, 30, 1), U = (int)bits(w, 29, 1), sz = (int)bits(w, 22, 2),
        opc = (int)bits(w, 12, 5), n = (int)bits(w, 5, 5), d = (int)bits(w, 0, 5);
    if (opc >= 0b11000 || opc == 0b10110 || opc == 0b10111 || (0b01100 <= opc && opc <= 0b01111))
        return misc2_fp(w, scalar);
    if (scalar) return misc2_scalar(U, sz, opc, d, n);
    int ne = lanes(Q, sz);
    std::string T = vu(sz), S = vs(sz);
    if (opc == 0 || (opc == 1 && !U)) {  // REV64 / REV32 / REV16
        int gsz;
        if (U == 0 && opc == 0) gsz = 3;
        else if (U == 1 && opc == 0) gsz = 2;
        else gsz = 1;
        if (sz >= gsz) throw Unsupported("rev");
        int es = 1 << sz, gb = 1 << gsz;
        std::vector<int> idx;
        for (int i = 0; i < 16; i++) {
            int g = i / gb * gb, o = i % gb;
            int e = o / es, b = o % es;
            idx.push_back(g + (gb / es - 1 - e) * es + b);
        }
        return VWr("vu8", Q, d, shuf("vu8", VR("vu8", n), VR("vu8", n), idx));
    }
    if (sz == 3 && !Q && opc != 0b10010 && opc != 0b10100 && opc != 0b10011)
        throw Unsupported("misc 1d");
    if (opc == 2 || opc == 6) {
        if (sz == 3) throw Unsupported("addlp size");
        return "sl_addlp(c, " + std::to_string(U) + ", " + std::to_string(opc == 6) + ", " +
               std::to_string(Q) + ", " + std::to_string(sz) + ", " + std::to_string(d) + ", " +
               std::to_string(n) + ");";
    }
    if (opc == 3) return same(lop(U ? "USQADD" : "SUQADD"), sz, ne, d, n);
    if (opc == 4) {
        if (sz == 3) throw Unsupported("clz size");
        return same(lop(U ? "CLZ" : "CLS"), sz, ne, d, n);
    }
    if (opc == 5) {
        if (!U) {
            if (sz) throw Unsupported("cnt size");
            return same(lop("CNT"), 0, ne, d, n);
        }
        if (sz == 0) return VWr("vu64", Q, d, "~" + VR("vu64", n));
        if (sz == 1) return same(lop("RBIT"), 0, lanes(Q, 0), d, n);
        throw Unsupported("not/rbit size");
    }
    if (opc == 7) return same(lop(U ? "SQNEG" : "SQABS"), sz, ne, d, n);
    std::string a = VR(T, n), sa = VR(S, n);
    if (opc == 8) return VWr(T, Q, d, "(" + T + ")(" + sa + (U ? " >= " : " > ") + "0)");
    if (opc == 9) return VWr(T, Q, d, "(" + T + ")(" + sa + (U ? " <= " : " == ") + "0)");
    if (opc == 10 && !U) return VWr(T, Q, d, "(" + T + ")(" + sa + " < 0)");
    if (opc == 11) {
        if (U) return VWr(T, Q, d, "-" + a);
        return VWr(T, Q, d, "((" + T + ")" + sa + " ^ (" + T + ")(" + sa + " >> " +
                                  std::to_string((8 << sz) - 1) + ")) - (" + T + ")(" + sa + " >> " +
                                  std::to_string((8 << sz) - 1) + ")");
    }
    if (opc == 0b10010 || opc == 0b10100) {  // XTN / SQXTUN / SQXTN / UQXTN
        if (sz == 3) throw Unsupported("xtn size");
        const char *op;
        if (opc == 0b10010 && U == 0) op = "XTN";
        else if (opc == 0b10010 && U == 1) op = "SQXTUN";
        else if (opc == 0b10100 && U == 0) op = "SQXTN";
        else op = "UQXTN";
        ne = 8 >> sz;
        return gen(lop(op), sz, ne, d, sz, Q * ne, Q, n, sz + 1, 0);
    }
    if (opc == 0b10011 && U) {  // SHLL
        if (sz == 3) throw Unsupported("shll size");
        ne = 8 >> sz;
        return gen(lop("SHLL"), sz, ne, d, sz + 1, 0, 0, n, sz, Q * ne);
    }
    throw Unsupported("misc2 opcode");
}

std::string SimdC::misc2_scalar(int U, int sz, int opc, int d, int n) {
    if (opc == 3) return same(lop(U ? "USQADD" : "SUQADD"), sz, 1, d, n);
    if (opc == 7) return same(lop(U ? "SQNEG" : "SQABS"), sz, 1, d, n);
    if (opc == 0b10010 || opc == 0b10100) {
        if (sz == 3 || (opc == 0b10010 && !U)) throw Unsupported("scalar xtn");
        const char *op;
        if (opc == 0b10010 && U == 1) op = "SQXTUN";
        else if (opc == 0b10100 && U == 0) op = "SQXTN";
        else op = "UQXTN";
        return gen(lop(op), sz, 1, d, sz, 0, 0, n, sz + 1, 0);
    }
    if (sz != 3) throw Unsupported("scalar misc size");
    std::string a = "VS64(" + std::to_string(n) + ")", sa = "(int64_t)VS64(" + std::to_string(n) + ")";
    if (opc == 8) return "c->v[" + std::to_string(d) + "] = (" + sa + (U ? " >= " : " > ") + "0) ? ~0ull : 0;";
    if (opc == 9) return "c->v[" + std::to_string(d) + "] = (" + sa + (U ? " <= " : " == ") + "0) ? ~0ull : 0;";
    if (opc == 10 && !U) return "c->v[" + std::to_string(d) + "] = (" + sa + " < 0) ? ~0ull : 0;";
    if (opc == 11)
        return U ? "c->v[" + std::to_string(d) + "] = (uint64_t)-" + a + ";"
                 : "c->v[" + std::to_string(d) + "] = (" + sa + " < 0) ? (uint64_t)-" + a + " : " + a + ";";
    throw Unsupported("scalar misc opcode");
}

std::string SimdC::misc2_fp(uint32_t w, bool scalar) {
    int Q = (int)bits(w, 30, 1), U = (int)bits(w, 29, 1), a_ = (int)bits(w, 23, 1),
        sz = (int)bits(w, 22, 1), opc = (int)bits(w, 12, 5), n = (int)bits(w, 5, 5),
        d = (int)bits(w, 0, 5);
    const FT_t &ft = FT[sz];
    std::string F = ft.F, VT = ft.VT, VU = ft.VU, T = ft.T, SF = ft.SF, SU = ft.SU, SET = ft.SET;
    if (opc == 0b10110 || opc == 0b10111) {  // FCVTN / FCVTL / FCVTXN
        if (a_) throw Unsupported("fcvtn a");
        if (opc == 0b10110 && U) {
            if (!sz) throw Unsupported("fcvtxn size");
            if (scalar) return "FP_SETS(" + std::to_string(d) + ", simd_d2s_odd(c, VSF64(" +
                               std::to_string(n) + ")));";
            return "sv_fcvtn(c, 1, " + std::to_string(Q) + ", 1, " + std::to_string(d) + ", " +
                   std::to_string(n) + ");";
        }
        if (scalar || U) throw Unsupported("fcvtn/l");
        if (opc == 0b10110)
            return "sv_fcvtn(c, " + std::to_string(sz) + ", " + std::to_string(Q) + ", 0, " +
                   std::to_string(d) + ", " + std::to_string(n) + ");";
        return "sv_fcvtl(c, " + std::to_string(sz) + ", " + std::to_string(Q) + ", " + std::to_string(d) +
               ", " + std::to_string(n) + ");";
    }
    if (!scalar && sz && !Q) throw Unsupported("fp misc 2d Q");
    std::string x = VQ(VT, Q, n);
    if (0b01100 <= opc && opc <= 0b01111) {
        if (!a_) throw Unsupported("fp16 misc");
        if (opc == 0b01111) {  // FABS / FNEG
            if (scalar) throw Unsupported("scalar fabs");
            std::string mask = sz ? "0x8000000000000000ull" : "0x80000000u";
            return VWr(VU, Q, d, U ? VR(VU, n) + " ^ " + mask : VR(VU, n) + " & ~" + mask);
        }
        int cop, swap;
        if (opc == 0b01100 && U == 0) { cop = 2; swap = 0; }
        else if (opc == 0b01100 && U == 1) { cop = 1; swap = 0; }
        else if (opc == 0b01101 && U == 0) { cop = 0; swap = 0; }
        else if (opc == 0b01101 && U == 1) { cop = 1; swap = 1; }
        else if (opc == 0b01110 && U == 0) { cop = 2; swap = 1; }
        else throw Unsupported("fcm zero");
        if (scalar) {
            std::string a = "(" + VT + "){" + SF + "(" + std::to_string(n) + ")}", b = "(" + VT + "){}";
            if (swap) std::swap(a, b);
            return "c->v[" + std::to_string(d) + "] = (" + ut(2 + sz) + ")vcmp_" + F + "(c, " +
                   std::to_string(cop) + ", " + a + ", " + b + ")[0];";
        }
        std::string a = x, b = "(" + VT + "){}";
        if (swap) std::swap(a, b);
        return VWr(VU, Q, d, "vcmp_" + F + "(c, " + std::to_string(cop) + ", " + a + ", " + b + ")");
    }
    if (opc == 0b11111) {
        if (!a_) throw Unsupported("fp misc 11111");
        if (U) {
            if (scalar) throw Unsupported("scalar fsqrt");
            return "SV_FOP_" + up(F) +
                   "(FOP_SQRT, " + std::to_string(Q) + ", " + std::to_string(d) + ", " + x + ", " + x +
                   ", (" + VT + "){});";
        }
        if (!scalar) throw Unsupported("vector frecpx");
        return SET + "(" + std::to_string(d) + ", recpx_" + F + "(c, " + SF + "(" + std::to_string(n) +
               ")));";
    }
    if (opc == 0b11000 || opc == 0b11001) {  // FRINT*
        if (scalar) throw Unsupported("scalar frint");
        int rm;
        if (U == 0 && a_ == 0 && (opc & 1) == 0) rm = 0;
        else if (U == 0 && a_ == 0 && (opc & 1) == 1) rm = 2;
        else if (U == 0 && a_ == 1 && (opc & 1) == 0) rm = 1;
        else if (U == 0 && a_ == 1 && (opc & 1) == 1) rm = 3;
        else if (U == 1 && a_ == 0 && (opc & 1) == 0) rm = 4;
        else if (U == 1 && a_ == 0 && (opc & 1) == 1) rm = 5;
        else if (U == 1 && a_ == 1 && (opc & 1) == 1) rm = 6;
        else throw Unsupported("frint");
        return VWr(VT, Q, d, "vfrint_" + F + "(c, " + x + ", " + std::to_string(rm) + ")");
    }
    if (opc == 0b11010 || opc == 0b11011 || (opc == 0b11100 && !a_)) {  // FCVT[NPMZA][SU]
        int rm;
        if (opc == 0b11010 && a_ == 0) rm = 0;
        else if (opc == 0b11010 && a_ == 1) rm = 1;
        else if (opc == 0b11011 && a_ == 0) rm = 2;
        else if (opc == 0b11011 && a_ == 1) rm = 3;
        else rm = 4;
        if (scalar)
            return "c->v[" + std::to_string(d) + "] = (" + ut(2 + sz) + ")toint_" + F + "(c, " + SF + "(" +
                   std::to_string(n) + "), " + std::to_string(rm) + ", 0, " + std::to_string(32 << sz) +
                   ", " + std::to_string(U) + ");";
        return VWr(VU, Q, d, "vtoint_" + F + "(c, " + x + ", " + std::to_string(rm) + ", 0, " +
                                  std::to_string(U) + ")");
    }
    if (opc == 0b11100) {  // URECPE / URSQRTE
        if (scalar || sz) throw Unsupported("urecpe");
        return same(lop(U ? "URSQRTE" : "URECPE"), 2, lanes(Q, 2), d, n);
    }
    if (opc == 0b11101) {
        if (!a_) {  // SCVTF / UCVTF
            if (scalar)
                return SET + "(" + std::to_string(d) + ", fromint_" + F + "(c, " + SU + "(" +
                       std::to_string(n) + "), " + std::to_string(32 << sz) + ", " + std::to_string(U) +
                       ", 0));";
            return VWr(VT, Q, d, "vfromint_" + F + "(c, " + VR(VU, n) + ", 0, " + std::to_string(U) + ")");
        }
        if (scalar)
            return SET + "(" + std::to_string(d) + ", " + (U ? "rsqrte" : "recpe") + "_" + F + "(c, " +
                   SF + "(" + std::to_string(n) + ")));";
        return VWr(VT, Q, d, "v" + std::string(U ? "rsqrte" : "recpe") + "_" + F + "(c, " + x + ")");
    }
    throw Unsupported("fp misc opcode");
}

// ------------------------------------------------------------ across lanes / pairwise

std::string SimdC::across(uint32_t w) {
    int Q = (int)bits(w, 30, 1), U = (int)bits(w, 29, 1), sz = (int)bits(w, 22, 2),
        opc = (int)bits(w, 12, 5), n = (int)bits(w, 5, 5), d = (int)bits(w, 0, 5);
    if (opc == 0b01100 || opc == 0b01111) {
        if (!U) throw Unsupported("fp16 across");
        if (!Q || (sz & 1)) throw Unsupported("fp across size");
        const char *fop;
        if (opc == 0b01100 && (sz >> 1) == 0) fop = "FOP_MAXNM";
        else if (opc == 0b01100 && (sz >> 1) == 1) fop = "FOP_MINNM";
        else if (opc == 0b01111 && (sz >> 1) == 0) fop = "FOP_MAX";
        else fop = "FOP_MIN";
        return "sv_fred_s(c, " + std::string(fop) + ", " + std::to_string(d) + ", " + std::to_string(n) + ");";
    }
    int op;
    if (opc == 0b00011) op = 1;
    else if (opc == 0b01010) op = 2;
    else if (opc == 0b11010) op = 3;
    else if (opc == 0b11011) op = 0;
    else op = -1;
    if (op < 0 || (op == 0 && U) || sz == 3 || (sz == 2 && !Q)) throw Unsupported("across");
    return "sl_across(c, " + std::to_string(op) + ", " + std::to_string(U) + ", " + std::to_string(Q) +
           ", " + std::to_string(sz) + ", " + std::to_string(d) + ", " + std::to_string(n) + ");";
}

std::string SimdC::scalar_pairwise(uint32_t w) {
    int U = (int)bits(w, 29, 1), sz = (int)bits(w, 22, 2), opc = (int)bits(w, 12, 5),
        n = (int)bits(w, 5, 5), d = (int)bits(w, 0, 5);
    if (!U) {
        if (opc == 0b11011 && sz == 3)
            return "c->v[" + std::to_string(d) + "] = (uint64_t)(VS64(" + std::to_string(n) + ") + VHI64(" +
                   std::to_string(n) + "));";
        throw Unsupported("scalar pairwise");
    }
    const char *fop;
    if (opc == 0b01100 && (sz >> 1) == 0) fop = "FOP_MAXNM";
    else if (opc == 0b01100 && (sz >> 1) == 1) fop = "FOP_MINNM";
    else if (opc == 0b01101 && (sz >> 1) == 0) fop = "FOP_ADD";
    else if (opc == 0b01111 && (sz >> 1) == 0) fop = "FOP_MAX";
    else if (opc == 0b01111 && (sz >> 1) == 1) fop = "FOP_MIN";
    else throw Unsupported("scalar fp pairwise");
    const FT_t &ft = FT[sz & 1];
    return std::string(ft.SET) + "(" + std::to_string(d) + ", pair_" + ft.F + "(c, " + fop + ", " +
           VR(ft.VT, n) + "[0], " + VR(ft.VT, n) + "[1]));";
}

// ------------------------------------------------------------ modified immediate

std::string SimdC::modimm(uint32_t w) {
    int Q = (int)bits(w, 30, 1), op = (int)bits(w, 29, 1), cmode = (int)bits(w, 12, 4),
        o2 = (int)bits(w, 11, 1), d = (int)bits(w, 0, 5);
    int imm8 = (int)((bits(w, 16, 3) << 5) | bits(w, 5, 5));
    if (o2) throw Unsupported("fmov half imm");
    if (cmode == 15 && op && !Q) throw Unsupported("fmov 1d");
    uint64_t imm = simd_expand_imm(op, cmode, imm8);
    int c4 = cmode;
    if ((c4 < 8 && (c4 & 1)) || (8 <= c4 && c4 < 12 && (c4 & 1))) {  // ORR / BIC
        if (op) {
            imm = ~imm & ones(64);
            return VWr("vu64", Q, d, VR("vu64", d) + " & " + hx(imm));
        }
        return VWr("vu64", Q, d, VR("vu64", d) + " | " + hx(imm));
    }
    if (op && !(c4 == 14 || c4 == 15)) imm = ~imm & ones(64);  // MVNI
    __uint128_t v = Q ? ((__uint128_t)imm << 64 | imm) : (__uint128_t)imm;
    return "c->v[" + std::to_string(d) + "] = " + u128(v) + ";";
}

// ------------------------------------------------------------ shift by immediate

std::string SimdC::shift_imm(uint32_t w, bool scalar) {
    int Q = (int)bits(w, 30, 1), U = (int)bits(w, 29, 1), immh = (int)bits(w, 19, 4),
        immb = (int)bits(w, 16, 3), opc = (int)bits(w, 11, 5), n = (int)bits(w, 5, 5),
        d = (int)bits(w, 0, 5);
    if (!immh) throw Unsupported("shift immh");
    int sz = 0;
    for (int t = immh; t; t >>= 1) sz++;
    sz--;
    int es = 8 << sz;
    int ih = (immh << 3) | immb;
    int rsh = 2 * es - ih, lsh = ih - es;
    if (scalar) Q = 1;
    int ne = scalar ? 1 : lanes(Q, sz);
    bool narrow = 0b10000 <= opc && opc <= 0b10011;
    if (opc == 0b11100 || opc == 0b11111) {  // fixed-point conversions
        if (sz < 2) throw Unsupported("fp16 fixed");
        if (sz == 3 && !Q) throw Unsupported("fixed 1d");
        const FT_t &ft = FT[sz - 2];
        int fb = rsh;
        if (opc == 0b11100) {
            if (scalar)
                return std::string(ft.SET) + "(" + std::to_string(d) + ", fromint_" + ft.F + "(c, " + ft.SU +
                       "(" + std::to_string(n) + "), " + std::to_string(es) + ", " + std::to_string(U) +
                       ", " + std::to_string(fb) + "));";
            return VWr(ft.VT, Q, d, std::string("vfromint_") + ft.F + "(c, " + VR(ft.VU, n) + ", " +
                                      std::to_string(fb) + ", " + std::to_string(U) + ")");
        }
        if (scalar)
            return "c->v[" + std::to_string(d) + "] = (" + ut(sz) + ")toint_" + ft.F + "(c, " + ft.SF + "(" +
                   std::to_string(n) + "), 3, " + std::to_string(fb) + ", " + std::to_string(es) + ", " +
                   std::to_string(U) + ");";
        return VWr(ft.VU, Q, d, std::string("vtoint_") + ft.F + "(c, " + VQ(ft.VT, Q, n) + ", 3, " +
                                  std::to_string(fb) + ", " + std::to_string(U) + ")");
    }
    if (narrow) {
        if (sz == 3) throw Unsupported("narrow shift size");
        ne = scalar ? 1 : 8 >> sz;
        const char *op;
        int u;
        if (opc == 0b10000 && U == 0) { op = "SHRN"; u = 0; }
        else if (opc == 0b10001 && U == 0) { op = "RSHRN"; u = 0; }
        else if (opc == 0b10000 && U == 1) { op = "QSHRUN"; u = 0; }
        else if (opc == 0b10001 && U == 1) { op = "QRSHRUN"; u = 0; }
        else if (opc == 0b10010 && U == 0) { op = "QSHRN"; u = 0; }
        else if (opc == 0b10010 && U == 1) { op = "QSHRN"; u = 1; }
        else if (opc == 0b10011 && U == 0) { op = "QRSHRN"; u = 0; }
        else { op = "QRSHRN"; u = 1; }
        if (scalar && (std::string(op) == "SHRN" || std::string(op) == "RSHRN"))
            throw Unsupported("scalar shrn");
        int part = scalar ? 0 : Q;
        return gen(lop(op, u), sz, ne, d, sz, part * ne, part, n, sz + 1, 0, -1, 0, 0, -1, rsh);
    }
    if (opc == 0b10100) {  // SSHLL / USHLL
        if (scalar || sz == 3) throw Unsupported("sshll");
        ne = 8 >> sz;
        return gen(lop("SHLLI", U), sz, ne, d, sz + 1, 0, 0, n, sz, Q * ne, -1, 0, 0, -1, lsh);
    }
    if (sz == 3 && !Q) throw Unsupported("shift 1d");
    if (scalar && sz != 3 && opc != 0b01100 && opc != 0b01110) throw Unsupported("scalar shift size");
    std::string T = vu(sz), S = vs(sz);
    if (opc == 0 || opc == 2) {  // SSHR/USHR, SSRA/USRA
        std::string e;
        if (U) e = rsh >= es ? "(" + T + "){}" : "(" + VR(T, n) + " >> " + std::to_string(rsh) + ")";
        else e = "(" + T + ")(" + VR(S, n) + " >> " + std::to_string(rsh < es ? rsh : es - 1) + ")";
        if (opc == 2) e = VR(T, d) + " + " + e;
        if (scalar) return "c->v[" + std::to_string(d) + "] = (uint64_t)((" + e + ")[0]);";
        return VWr(T, Q, d, e);
    }
    if (opc == 4 || opc == 6) return same(lop(opc == 4 ? "RSHRI" : "RSRAI", U), sz, ne, d, n, -1, rsh);
    if (opc == 8) {
        if (!U) throw Unsupported("sri U");
        return same(lop("SRI"), sz, ne, d, n, -1, rsh);
    }
    if (opc == 10) {
        if (U) return same(lop("SLI"), sz, ne, d, n, -1, lsh);
        std::string e = VR(T, n) + " << " + std::to_string(lsh);
        if (scalar) return "c->v[" + std::to_string(d) + "] = (uint64_t)((" + e + ")[0]);";
        return VWr(T, Q, d, e);
    }
    if (opc == 12) {
        if (!U) throw Unsupported("sqshlu U");
        return same(lop("QSHLUI"), sz, ne, d, n, -1, lsh);
    }
    if (opc == 14) return same(lop("QSHLI", U), sz, ne, d, n, -1, lsh);
    throw Unsupported("shift imm opcode");
}

// ------------------------------------------------------------ indexed element

std::string SimdC::indexed(uint32_t w, bool scalar) {
    int Q = (int)bits(w, 30, 1), U = (int)bits(w, 29, 1), sz = (int)bits(w, 22, 2),
        L = (int)bits(w, 21, 1), M = (int)bits(w, 20, 1), rm = (int)bits(w, 16, 4),
        opc = (int)bits(w, 12, 4), H = (int)bits(w, 11, 1), n = (int)bits(w, 5, 5),
        d = (int)bits(w, 0, 5);
    bool fp = (opc == 1 || opc == 5 || opc == 9) && !(U && opc != 9);
    if (fp) {
        if (sz < 2) throw Unsupported("fp16 indexed");
        int fsz = sz & 1;
        int idx, m;
        if (fsz) {
            if (L) throw Unsupported("indexed L");
            idx = H;
            m = (M << 4) | rm;
        } else {
            idx = (H << 1) | L;
            m = (M << 4) | rm;
        }
        if (!scalar && fsz && !Q) throw Unsupported("indexed 2d Q");
        const FT_t &ft = FT[fsz];
        std::string F = ft.F, VT = ft.VT, VU = ft.VU, T = ft.T, SF = ft.SF, SU = ft.SU, SET = ft.SET;
        if (scalar) {
            std::string el = VR(VT, m) + "[" + std::to_string(idx) + "]";
            if (opc == 9)
                return SET + "(" + std::to_string(d) + ", op_" + F + "(c, " +
                       (U ? "FOP_MULX" : "FOP_MUL") + ", " + SF + "(" + std::to_string(n) + "), " + el +
                       ", 0));";
            std::string x = opc == 1 ? SF + "(" + std::to_string(n) + ")"
                                     : "neg_" + F + "(" + SF + "(" + std::to_string(n) + "))";
            return SET + "(" + std::to_string(d) + ", op_" + F + "(c, FOP_MADD, " + SF + "(" +
                   std::to_string(d) + "), " + x + ", " + el + "));";
        }
        std::string el = "vsplat_" + F + "(" + VR(VT, m) + ", " + std::to_string(idx) + ", " +
                         std::to_string(Q) + ")";
        if (opc == 9)
            return "SV_FOP_" + up(F) + "(" +
                   (U ? "FOP_MULX" : "FOP_MUL") + ", " + std::to_string(Q) + ", " + std::to_string(d) +
                   ", " + VQ(VT, Q, n) + ", " + el + ", (" + VT + "){});";
        std::string x = opc == 1 ? VQ(VT, Q, n) : "vneg_" + F + "(" + VQ(VT, Q, n) + ")";
        return "SV_FOP_" + up(F) +
               "(FOP_MADD, " + std::to_string(Q) + ", " + std::to_string(d) + ", " + VQ(VT, Q, d) + ", " +
               x + ", " + el + ");";
    }
    int idx, m;
    if (sz == 1) {
        idx = (H << 2) | (L << 1) | M;
        m = rm;
    } else if (sz == 2) {
        idx = (H << 1) | L;
        m = (M << 4) | rm;
    } else {
        throw Unsupported("indexed size");
    }
    std::string T = vu(sz);
    if (scalar) {
        const char *op = nullptr;
        if (U == 0 && opc == 3) op = "SQDMLAL";
        else if (U == 0 && opc == 7) op = "SQDMLSL";
        else if (U == 0 && opc == 11) op = "SQDMULL";
        else if (U == 0 && opc == 12) op = "SQDMULH";
        else if (U == 0 && opc == 13) op = "SQRDMULH";
        if (!op) throw Unsupported("scalar indexed");
        if (std::string(op) == "SQDMULH" || std::string(op) == "SQRDMULH")
            return gen(lop(op), sz, 1, d, sz, 0, 0, n, sz, 0, m, sz, 0, idx);
        return gen(lop(op), sz, 1, d, sz + 1, 0, 0, n, sz, 0, m, sz, 0, idx);
    }
    int ne = lanes(Q, sz);
    std::vector<int> idxs(16 >> sz, idx);
    std::string el = shuf(T, VR(T, m), VR(T, m), idxs);
    if (U == 0 && opc == 8) return VWr(T, Q, d, VR(T, n) + " * " + el);
    if (U == 1 && (opc == 0 || opc == 4))
        return VWr(T, Q, d, VR(T, d) + (opc == 0 ? " + " : " - ") + VR(T, n) + " * " + el);
    if (U == 0 && (opc == 12 || opc == 13))
        return same(lop(opc == 12 ? "SQDMULH" : "SQRDMULH"), sz, ne, d, n, m, 0, idx);
    const char *lng = nullptr;
    if (opc == 2) lng = "MLAL";
    else if (opc == 6) lng = "MLSL";
    else if (opc == 10) lng = "MULL";
    else if (opc == 3) lng = "SQDMLAL";
    else if (opc == 7) lng = "SQDMLSL";
    else if (opc == 11) lng = "SQDMULL";
    if (!lng || (U && std::string(lng).substr(0, 2) == "SQ")) throw Unsupported("indexed opcode");
    ne = 8 >> sz;
    int u = std::string(lng).substr(0, 2) == "SQ" ? 0 : U;
    return gen(lop(lng, u), sz, ne, d, sz + 1, 0, 0, n, sz, Q * ne, m, sz, 0, idx);
}

// ------------------------------------------------------------ scalar FP

static int ftype(uint32_t w) {
    int t = (int)bits(w, 22, 2);
    if (t > 1) throw Unsupported("fp type");
    return t;
}

std::string SimdC::fp_dp3(uint32_t w) {
    int t = ftype(w);
    int o1 = (int)bits(w, 21, 1), o0 = (int)bits(w, 15, 1);
    int d = (int)bits(w, 0, 5), n = (int)bits(w, 5, 5), m = (int)bits(w, 16, 5),
        a = (int)bits(w, 10, 5);
    std::string F = FT[t].F;
    int na = o1, nn = o1 ^ o0;
    return "fp_fma_" + F + "(c, " + std::to_string(na) + ", " + std::to_string(nn) + ", " +
           std::to_string(d) + ", " + std::to_string(n) + ", " + std::to_string(m) + ", " +
           std::to_string(a) + ");";
}

std::string SimdC::fp_dp2(uint32_t w) {
    int t = ftype(w);
    int opc = (int)bits(w, 12, 4), d = (int)bits(w, 0, 5), n = (int)bits(w, 5, 5),
        m = (int)bits(w, 16, 5);
    static const char *fop[9] = {"FOP_MUL", "FOP_DIV", "FOP_ADD", "FOP_SUB", "FOP_MAX",
                                 "FOP_MIN", "FOP_MAXNM", "FOP_MINNM", "FOP_NMUL"};
    if (opc > 8) throw Unsupported("fp dp2");
    return "fp_op_" + std::string(FT[t].F) + "(c, " + fop[opc] + ", " + std::to_string(d) + ", " +
           std::to_string(n) + ", " + std::to_string(m) + ");";
}

std::string SimdC::fp_dp1(uint32_t w) {
    int t = (int)bits(w, 22, 2);
    int opc = (int)bits(w, 15, 6), d = (int)bits(w, 0, 5), n = (int)bits(w, 5, 5);
    if (t == 3) {  // half source: only FCVT
        if (opc == 4) return "FP_SETS(" + std::to_string(d) + ", simd_h2s(c, VS16(" + std::to_string(n) + ")));";
        if (opc == 5) return "FP_SETD(" + std::to_string(d) + ", simd_h2d(c, VS16(" + std::to_string(n) + ")));";
        throw Unsupported("fp16 dp1");
    }
    if (t == 2) throw Unsupported("fp type");
    const FT_t &ft = FT[t];
    std::string F = ft.F, VT = ft.VT, VU = ft.VU, T = ft.T, SF = ft.SF, SU = ft.SU, SET = ft.SET;
    std::string sign = t ? "0x8000000000000000ull" : "0x80000000u";
    if (opc == 0) return "c->v[" + std::to_string(d) + "] = " + SU + "(" + std::to_string(n) + ");";
    if (opc == 1) return "c->v[" + std::to_string(d) + "] = " + SU + "(" + std::to_string(n) + ") & ~" + sign + ";";
    if (opc == 2) return "c->v[" + std::to_string(d) + "] = " + SU + "(" + std::to_string(n) + ") ^ " + sign + ";";
    if (opc == 3)
        return "fp_op_" + F + "(c, FOP_SQRT, " + std::to_string(d) + ", " + std::to_string(n) + ", " +
               std::to_string(n) + ");";
    if (opc == 4 || opc == 5 || opc == 7) {
        int to = opc == 4 ? 0 : (opc == 5 ? 1 : 3);
        if (to == t) throw Unsupported("fcvt same");
        if (to == 3)
            return "c->v[" + std::to_string(d) + "] = " + std::string(t ? "simd_dd2h" : "simd_s2h") +
                   "(c, " + SF + "(" + std::to_string(n) + "));";
        if (to)
            return "FP_SETD(" + std::to_string(d) + ", simd_s2d(c, VSF32(" + std::to_string(n) + ")));";
        return "FP_SETS(" + std::to_string(d) + ", simd_d2s(c, VSF64(" + std::to_string(n) + ")));";
    }
    int rm;
    if (opc == 8) rm = 0;
    else if (opc == 9) rm = 1;
    else if (opc == 10) rm = 2;
    else if (opc == 11) rm = 3;
    else if (opc == 12) rm = 4;
    else if (opc == 14) rm = 5;
    else if (opc == 15) rm = 6;
    else throw Unsupported("fp dp1 opcode");
    return SET + "(" + std::to_string(d) + ", frint_" + F + "(c, " + SF + "(" + std::to_string(n) + "), " +
           std::to_string(rm) + "));";
}

std::string SimdC::fp_cmp(uint32_t w) {
    int t = ftype(w);
    if (bits(w, 14, 2) || bits(w, 0, 3)) throw Unsupported("fcmp op");
    const FT_t &ft = FT[t];
    int n = (int)bits(w, 5, 5), m = (int)bits(w, 16, 5), zero = (int)bits(w, 3, 1);
    std::string b = zero ? "0" : std::string(ft.SF) + "(" + std::to_string(m) + ")";
    return "c->nzcv = cmp_" + std::string(ft.F) + "(c, " + ft.SF + "(" + std::to_string(n) + "), " + b + ");";
}

std::string SimdC::fp_ccmp(uint32_t w) {
    int t = ftype(w);
    const FT_t &ft = FT[t];
    int n = (int)bits(w, 5, 5), m = (int)bits(w, 16, 5), cnd = (int)bits(w, 12, 4),
        nzcv = (int)bits(w, 0, 4);
    return "c->nzcv = " + cond(cnd) + " ? cmp_" + ft.F + "(c, " + ft.SF + "(" + std::to_string(n) + "), " +
           ft.SF + "(" + std::to_string(m) + ")) : " + hx((uint64_t)nzcv << 28) + ";";
}

std::string SimdC::fp_csel(uint32_t w) {
    int t = ftype(w);
    std::string SU = FT[t].SU;
    int d = (int)bits(w, 0, 5), n = (int)bits(w, 5, 5), m = (int)bits(w, 16, 5),
        cnd = (int)bits(w, 12, 4);
    return "c->v[" + std::to_string(d) + "] = " + cond(cnd) + " ? " + SU + "(" + std::to_string(n) + ") : " +
           SU + "(" + std::to_string(m) + ");";
}

std::string SimdC::fp_imm(uint32_t w) {
    int t = ftype(w);
    if (bits(w, 5, 5)) throw Unsupported("fmov imm5");
    int imm8 = (int)bits(w, 13, 8), d = (int)bits(w, 0, 5);
    return "c->v[" + std::to_string(d) + "] = " + hx(vfp_expand_imm(imm8, 32 << t)) + ";";
}

std::string SimdC::fp_fixed(uint32_t w) {
    bool sf = bits(w, 31, 1);
    int t = (int)bits(w, 22, 2), rmode = (int)bits(w, 19, 2), opc = (int)bits(w, 16, 3),
        scale = (int)bits(w, 10, 6), n = (int)bits(w, 5, 5), d = (int)bits(w, 0, 5);
    if (t > 1) throw Unsupported("fixed type");
    if (!sf && scale < 32) throw Unsupported("fixed scale");
    const FT_t &ft = FT[t];
    int fb = 64 - scale, ib = sf ? 64 : 32;
    if (rmode == 0 && (opc == 2 || opc == 3)) {
        std::string src = sf ? X(n) : W(n);
        return std::string(ft.SET) + "(" + std::to_string(d) + ", fromint_" + ft.F + "(c, " + src + ", " +
               std::to_string(ib) + ", " + std::to_string(opc & 1) + ", " + std::to_string(fb) + "));";
    }
    if (rmode == 3 && (opc == 0 || opc == 1))
        return set_reg(sf, d, std::string("toint_") + ft.F + "(c, " + ft.SF + "(" + std::to_string(n) + "), 3, " +
                                 std::to_string(fb) + ", " + std::to_string(ib) + ", " +
                                 std::to_string(opc) + ")");
    throw Unsupported("fixed opcode");
}

std::string SimdC::fp_int(uint32_t w) {
    bool sf = bits(w, 31, 1);
    int t = (int)bits(w, 22, 2), rmode = (int)bits(w, 19, 2), opc = (int)bits(w, 16, 3),
        n = (int)bits(w, 5, 5), d = (int)bits(w, 0, 5);
    if (opc == 6 || opc == 7) {  // FMOV
        if (t == 2 && sf && rmode == 1)
            return opc == 6 ? set_reg(true, d, "VHI64(" + std::to_string(n) + ")")
                            : "VHI64(" + std::to_string(d) + ") = " + X(n) + ";";
        if (rmode || t != (int)sf || t > 1) throw Unsupported("fmov general");
        if (opc == 6) return set_reg(sf, d, sf ? "VS64(" + std::to_string(n) + ")"
                                               : "VS32(" + std::to_string(n) + ")");
        return "c->v[" + std::to_string(d) + "] = " + (sf ? X(n) : W(n)) + ";";
    }
    if (t > 1) throw Unsupported("fp int type");
    const FT_t &ft = FT[t];
    int ib = sf ? 64 : 32;
    if (opc == 2 || opc == 3) {
        if (rmode) throw Unsupported("cvtf rmode");
        return std::string(ft.SET) + "(" + std::to_string(d) + ", fromint_" + ft.F + "(c, " +
               (sf ? X(n) : W(n)) + ", " + std::to_string(ib) + ", " + std::to_string(opc & 1) + ", 0));";
    }
    if (opc == 4 || opc == 5) {
        if (rmode) throw Unsupported("fcvta rmode");
        return set_reg(sf, d, std::string("toint_") + ft.F + "(c, " + ft.SF + "(" + std::to_string(n) + "), 4, 0, " +
                                 std::to_string(ib) + ", " + std::to_string(opc & 1) + ")");
    }
    static const int rm_tab[4] = {0, 1, 2, 3};
    int rm = rm_tab[rmode];
    return set_reg(sf, d, std::string("toint_") + ft.F + "(c, " + ft.SF + "(" + std::to_string(n) + "), " +
                             std::to_string(rm) + ", 0, " + std::to_string(ib) + ", " +
                             std::to_string(opc & 1) + ")");
}

// ------------------------------------------------------------ loads / stores

std::string SimdC::ldst(uint32_t w, uint64_t pc) {
    int rt = (int)bits(w, 0, 5);
    if (bits(w, 27, 3) == 0b011 && bits(w, 24, 2) == 0) {  // literal
        int opc = (int)bits(w, 30, 2);
        if (opc == 3) throw Unsupported("ldr literal opc");
        uint64_t addr = (pc + (uint64_t)sext(bits(w, 5, 19), 19) * 4) & ones(64);
        static const char *ld[3] = {"LD32", "LD64", "LD128"};
        return "c->v[" + std::to_string(rt) + "] = " + ld[opc] + "(" + hx(addr) + ");";
    }
    if (bits(w, 31, 1) == 0 && (bits(w, 24, 6) == 0b001100 || bits(w, 24, 6) == 0b001101))
        return ldst_struct(w);
    if (bits(w, 27, 3) == 0b101) return ldst_pair(w);
    if (bits(w, 27, 3) == 0b111) return ldst_reg(w);
    throw Unsupported("ldst simd");
}

std::string SimdC::ldst_pair(uint32_t w) {
    int opc = (int)bits(w, 30, 2), mode = (int)bits(w, 23, 2), L = (int)bits(w, 22, 1);
    int64_t imm7 = sext(bits(w, 15, 7), 7);
    int rt2 = (int)bits(w, 10, 5), rn = (int)bits(w, 5, 5), rt = (int)bits(w, 0, 5);
    if (opc == 3) throw Unsupported("pair opc 3");
    int scale = 2 + opc;
    int64_t off = imm7 << scale;
    std::string base = XS(rn);
    bool post = mode == 1, wb = mode == 1 || mode == 3;
    std::string ea = post ? base
                          : (off >= 0 ? base + " + " + std::to_string(off) : base + " - " + std::to_string(-off));
    static const char *ld[5] = {"", "", "LD32", "LD64", "LD128"};
    static const char *st[5] = {"", "", "ST32", "ST64", "ST128"};
    int sz = 1 << scale;
    std::vector<std::string> lines;
    lines.push_back("uint64_t ea = " + ea + ";");
    if (L) {
        lines.push_back("u128 a = " + std::string(ld[scale]) + "(ea), b = " + ld[scale] + "(ea + " +
                        std::to_string(sz) + ");");
        lines.push_back("c->v[" + std::to_string(rt) + "] = a; c->v[" + std::to_string(rt2) + "] = b;");
    } else {
        std::string v1, v2;
        if (scale == 4) {
            v1 = "c->v[" + std::to_string(rt) + "]";
            v2 = "c->v[" + std::to_string(rt2) + "]";
        } else {
            v1 = "VS" + std::to_string(8 << scale) + "(" + std::to_string(rt) + ")";
            v2 = "VS" + std::to_string(8 << scale) + "(" + std::to_string(rt2) + ")";
        }
        lines.push_back(std::string(st[scale]) + "(ea, " + v1 + "); " + st[scale] + "(ea + " +
                        std::to_string(sz) + ", " + v2 + ");");
    }
    if (wb) {
        std::string dst = rn == 31 ? "c->sp" : "c->x[" + std::to_string(rn) + "]";
        if (!post) lines.push_back(dst + " = ea;");
        else lines.push_back(off >= 0 ? dst + " = ea + " + std::to_string(off) + ";"
                                      : dst + " = ea - " + std::to_string(-off) + ";");
    }
    std::string body;
    for (size_t k = 0; k < lines.size(); k++) body += (k ? " " : "") + lines[k];
    return "{ " + body + " }";
}

std::string SimdC::ldst_reg(uint32_t w) {
    int size = (int)bits(w, 30, 2), opc = (int)bits(w, 22, 2), rn = (int)bits(w, 5, 5),
        rt = (int)bits(w, 0, 5);
    int scale;
    if (opc & 2) {
        if (size) throw Unsupported("ldst simd size");
        scale = 4;
    } else {
        scale = size;
    }
    bool L = opc & 1;
    std::string base = XS(rn);
    std::string wb, ea;
    if (bits(w, 24, 2) == 1) {
        ea = base + " + " + hx(bits(w, 10, 12) << scale);
    } else if (bits(w, 24, 2) == 0) {
        int kind = (int)bits(w, 10, 2);
        if (bits(w, 21, 1)) {
            if (kind != 2) throw Unsupported("ldst simd reg kind");
            int rm = (int)bits(w, 16, 5), option = (int)bits(w, 13, 3), S = (int)bits(w, 12, 1);
            if (option != 2 && option != 3 && option != 6 && option != 7)
                throw Unsupported("ldst simd reg option");
            ea = base + " + " + extend_expr(rm, option, S ? scale : 0);
        } else {
            int64_t imm9 = sext(bits(w, 12, 9), 9);
            std::string si = imm9 >= 0 ? "+ " + std::to_string(imm9) : "- " + std::to_string(-imm9);
            if (kind == 0) ea = base + " " + si;
            else if (kind == 1) { ea = base; wb = base + " " + si; }
            else if (kind == 3) { ea = base + " " + si; wb = "ea"; }
            else throw Unsupported("ldst simd unprivileged");
        }
    } else {
        throw Unsupported("ldst simd");
    }
    static const char *ld[5] = {"LD8", "LD16", "LD32", "LD64", "LD128"};
    static const char *st[5] = {"ST8", "ST16", "ST32", "ST64", "ST128"};
    std::vector<std::string> parts;
    parts.push_back("uint64_t ea = " + ea + ";");
    if (L) parts.push_back("c->v[" + std::to_string(rt) + "] = " + ld[scale] + "(ea);");
    else {
        std::string src = scale == 4 ? "c->v[" + std::to_string(rt) + "]"
                                     : "VS" + std::to_string(8 << scale) + "(" + std::to_string(rt) + ")";
        parts.push_back(std::string(st[scale]) + "(ea, " + src + ");");
    }
    if (!wb.empty()) parts.push_back((rn == 31 ? "c->sp" : "c->x[" + std::to_string(rn) + "]") + " = " + wb + ";");
    std::string body;
    for (size_t k = 0; k < parts.size(); k++) body += (k ? " " : "") + parts[k];
    return "{ " + body + " }";
}

std::string SimdC::ldst_struct(uint32_t w) {
    int Q = (int)bits(w, 30, 1), L = (int)bits(w, 22, 1), post = (int)bits(w, 23, 1),
        rm = (int)bits(w, 16, 5), rn = (int)bits(w, 5, 5), rt = (int)bits(w, 0, 5),
        sz = (int)bits(w, 10, 2);
    if (!post && rm) throw Unsupported("ldst struct rm");
    std::string base = XS(rn);
    std::string acc;
    int total = 0;
    if (bits(w, 24, 1) == 0) {  // multiple structures
        if (bits(w, 21, 1)) throw Unsupported("ldst multiple bit21");
        int opc = (int)bits(w, 12, 4);
        int selem, rpt;
        if (opc == 0) { selem = 4; rpt = 1; }
        else if (opc == 2) { selem = 1; rpt = 4; }
        else if (opc == 4) { selem = 3; rpt = 1; }
        else if (opc == 6) { selem = 1; rpt = 3; }
        else if (opc == 7) { selem = 1; rpt = 1; }
        else if (opc == 8) { selem = 2; rpt = 1; }
        else if (opc == 10) { selem = 1; rpt = 2; }
        else throw Unsupported("ldst multiple opcode");
        if (sz == 3 && !Q && selem > 1) throw Unsupported("ldst multiple 1d");
        total = (8 << Q) * selem * rpt;
        if (selem == 1) {  // LD1/ST1: whole registers
            int step = Q ? 16 : 8;
            std::string s;
            for (int i = 0; i < rpt; i++) {
                int r = (rt + i) % 32;
                if (L)
                    s += std::string(i ? " " : "") + "c->v[" + std::to_string(r) + "] = " +
                         std::string(Q ? "LD128" : "LD64") + "(ea + " + std::to_string(i * step) + ")";
                else
                    s += std::string(i ? " " : "") + std::string(Q ? "ST128" : "ST64") + "(ea + " +
                         std::to_string(i * step) + ", " + (Q ? "c->v[" + std::to_string(r) + "]"
                                                              : "VS64(" + std::to_string(r) + ")") + ")";
            }
            acc = s;
        } else {
            acc = "sl_" + std::string(L ? "ldm" : "stm") + "(c, " + std::to_string(Q) + ", " +
                  std::to_string(sz) + ", " + std::to_string(selem) + ", " + std::to_string(rpt) + ", " +
                  std::to_string(rt) + ", ea);";
        }
    } else {  // single structure
        int opc = (int)bits(w, 13, 3), S = (int)bits(w, 12, 1), R = (int)bits(w, 21, 1);
        int scale = opc >> 1;
        int selem = ((opc & 1) << 1 | R) + 1;
        if (scale == 3) {
            if (!L || S) throw Unsupported("ldnr");
            total = selem << sz;
            acc = "sl_ldr(c, " + std::to_string(Q) + ", " + std::to_string(sz) + ", " +
                  std::to_string(selem) + ", " + std::to_string(rt) + ", ea);";
        } else {
            int idx;
            if (scale == 0) idx = (Q << 3) | (S << 2) | sz;
            else if (scale == 1) {
                if (sz & 1) throw Unsupported("ld1 h size");
                idx = (Q << 2) | (S << 1) | (sz >> 1);
            } else {
                if (sz & 2) throw Unsupported("ld1 s size");
                if (sz & 1) {
                    if (S) throw Unsupported("ld1 d S");
                    idx = Q;
                    scale = 3;
                } else {
                    idx = (Q << 1) | S;
                }
            }
            total = selem << scale;
            acc = "sl_" + std::string(L ? "lds" : "sts") + "(c, " + std::to_string(scale) + ", " +
                  std::to_string(selem) + ", " + std::to_string(idx) + ", " + std::to_string(rt) + ", ea);";
        }
    }
    std::vector<std::string> parts;
    parts.push_back("uint64_t ea = " + base + ";");
    parts.push_back(acc);
    if (post) {
        std::string dst = rn == 31 ? "c->sp" : "c->x[" + std::to_string(rn) + "]";
        parts.push_back(rm == 31 ? dst + " = ea + " + std::to_string(total) + ";"
                                 : dst + " = ea + " + X(rm) + ";");
    }
    std::string body;
    for (size_t k = 0; k < parts.size(); k++) body += (k ? " " : "") + parts[k];
    return "{ " + body + " }";
}

// ------------------------------------------------------------ CRC32

std::string SimdC::crc(uint32_t w) {
    bool sf = bits(w, 31, 1);
    int rm = (int)bits(w, 16, 5), C = (int)bits(w, 12, 1), sz = (int)bits(w, 10, 2),
        rn = (int)bits(w, 5, 5), rd = (int)bits(w, 0, 5);
    if ((sz == 3) != sf) throw Unsupported("crc size");
    std::string val = sz == 3 ? X(rm) : W(rm);
    return set_reg(false, rd, std::string(C ? "CRC32C" : "CRC32") + "(" + W(rn) + ", " + val + ", " +
                                  std::to_string(1 << sz) + ")");
}

}  // namespace ngen
