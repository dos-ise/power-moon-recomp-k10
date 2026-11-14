// Shared AArch64 decode helpers and C-emission string builders (port of tools/lifter.py's
// module-level helpers). The generated C must match the Python generator byte for byte.
#pragma once

#include <cstdint>
#include <string>

namespace ngen {

// Cpu struct offsets (must match runtime/cpu.h)
enum : uint64_t {
    OFF_X = 0, OFF_SP = 256, OFF_PC = 264, OFF_NZCV = 272, OFF_V = 512,
};

static inline uint32_t bits(uint32_t w, int lo, int n) {
    if (n >= 32) return w >> lo;
    return (w >> lo) & ((1u << n) - 1);
}
static inline int64_t sext(int64_t v, int n) {
    return (v & (1ll << (n - 1))) ? v - (1ll << n) : v;
}
static inline uint64_t ones(int n) {
    return n >= 64 ? ~0ull : ((1ull << n) - 1);
}
static inline uint64_t ror(uint64_t v, int r, int size) {
    r %= size;
    if (!r) return v;
    return ((v >> r) | (v << (size - r))) & ones(size);
}
static inline uint64_t replicate(uint64_t v, int esize, int size) {
    uint64_t out = 0;
    for (int i = 0; i < size; i += esize) out |= v << i;
    return out;
}

// hex literal as the Python hx(): "0x<hex>ull"
static inline std::string hx(uint64_t v) {
    char buf[32];
    snprintf(buf, sizeof buf, "0x%llxull", (unsigned long long)v);
    return buf;
}
// bare hex (no prefix), as the Python f'{a:x}'
static inline std::string hex(uint64_t v) {
    char buf[24];
    snprintf(buf, sizeof buf, "%llx", (unsigned long long)v);
    return buf;
}

// decode_bit_masks (lifter.py): returns (wmask, tmask); throws on reserved encodings.
void decode_bit_masks(int N, int imms, int immr, int size, bool immediate, uint64_t &wmask,
                      uint64_t &tmask);

// ---- register expressions (must match lifter.py exactly)
static inline std::string X(int r) { return r == 31 ? "0ull" : "c->x[" + std::to_string(r) + "]"; }
static inline std::string XS(int r) { return r == 31 ? "c->sp" : "c->x[" + std::to_string(r) + "]"; }
static inline std::string W(int r) {
    return r == 31 ? "0u" : "(uint32_t)c->x[" + std::to_string(r) + "]";
}
static inline std::string WS(int r) {
    return r == 31 ? "(uint32_t)c->sp" : "(uint32_t)c->x[" + std::to_string(r) + "]";
}
static inline std::string R(bool sf, int r, bool sp = false) {
    if (sf) return sp ? XS(r) : X(r);
    return sp ? WS(r) : W(r);
}

// set_reg(sf, r, expr, sp): statement writing expr to Rd (zero-extending 32-bit results).
static inline std::string set_reg(bool sf, int r, const std::string &expr, bool sp = false) {
    if (r == 31 && !sp) return "(void)(" + expr + ");";
    std::string dst = r == 31 ? "c->sp" : "c->x[" + std::to_string(r) + "]";
    if (sf) return dst + " = (uint64_t)(" + expr + ");";
    return dst + " = (uint32_t)(" + expr + ");";
}

static inline std::string shift_expr(bool sf, const std::string &val, int typ, int amt) {
    if (amt == 0 && typ != 3) return val;
    if (typ == 0) return "(" + val + " << " + std::to_string(amt) + ")";
    if (typ == 1) return "(" + val + " >> " + std::to_string(amt) + ")";
    if (typ == 2)
        return sf ? "(uint64_t)((int64_t)" + val + " >> " + std::to_string(amt) + ")"
                  : "(uint32_t)((int32_t)" + val + " >> " + std::to_string(amt) + ")";
    return "ROR" + std::string(sf ? "64" : "32") + "(" + val + ", " + std::to_string(amt) + ")";
}

static inline std::string extend_expr(int r, int option, int shift) {
    std::string base = X(r);
    std::string e;
    switch (option) {
        case 0: e = "(uint64_t)(uint8_t)" + base; break;
        case 1: e = "(uint64_t)(uint16_t)" + base; break;
        case 2: e = "(uint64_t)(uint32_t)" + base; break;
        case 3: e = base; break;
        case 4: e = "(uint64_t)(int64_t)(int8_t)" + base; break;
        case 5: e = "(uint64_t)(int64_t)(int16_t)" + base; break;
        case 6: e = "(uint64_t)(int64_t)(int32_t)" + base; break;
        default: e = base; break;
    }
    return shift ? "(" + e + " << " + std::to_string(shift) + ")" : e;
}

}  // namespace ngen
