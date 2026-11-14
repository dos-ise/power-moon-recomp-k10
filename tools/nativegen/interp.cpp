#include "interp.h"

#include <cstring>

#include "arm64.h"

namespace ngen {

static const uint64_t SCRATCH_MEM = 0x7300000000ull;

Interp::Interp(const std::vector<uint8_t> &image, uint64_t base) : image_(image), base_(base) {}

uint32_t Interp::word(uint64_t a) const {
    uint32_t w;
    memcpy(&w, image_.data() + (a - base_), 4);
    return w;
}

uint64_t Interp::load(uint64_t a, int size) const {
    // Scratch memory (as Unicorn maps it): zeroed, so loads from it succeed.
    if (a >= SCRATCH_MEM && a + size <= SCRATCH_MEM + 0x100000) return 0;
    if (a < base_ || a + size > base_ + image_.size()) return 0;
    const uint8_t *p = image_.data() + (a - base_);
    uint64_t v = 0;
    memcpy(&v, p, size);
    return v;
}

static inline uint64_t &reg(uint64_t *x, uint64_t &sp, int r) { return r == 31 ? sp : x[r]; }

bool Interp::step(uint64_t &pc) {
    if (pc < base_ || pc + 4 > base_ + image_.size()) return false;
    uint32_t w = word(pc);
    uint64_t next = pc + 4;
    uint64_t &rd = reg(x, sp, (int)bits(w, 0, 5));
    int rn = (int)bits(w, 5, 5);
    int rm = (int)bits(w, 16, 5);

    // ---- PC-relative
    if ((w & 0x9F000000u) == 0x90000000u) {  // ADRP
        int64_t imm = sext((bits(w, 5, 19) << 2) | bits(w, 29, 2), 21);
        rd = (pc & ~0xFFFull) + (uint64_t)(imm << 12);
        pc = next;
        return true;
    }
    if ((w & 0x9F000000u) == 0x10000000u) {  // ADR
        int64_t imm = sext((bits(w, 5, 19) << 2) | bits(w, 29, 2), 21);
        rd = pc + (uint64_t)imm;
        pc = next;
        return true;
    }

    // ---- add/sub immediate
    if ((w & 0x7F000000u) == 0x11000000u) {  // ADD/SUB (imm, 32/64)
        bool sf = bits(w, 31, 1);
        bool sub = bits(w, 30, 1);
        bool S = bits(w, 29, 1);
        int sh = (int)bits(w, 22, 1);
        uint64_t imm = (uint64_t)bits(w, 10, 12) << (12 * sh);
        uint64_t a = reg(x, sp, rn);
        uint64_t r = sub ? a - imm : a + imm;
        if (S) {
            uint64_t res = r;
            uint64_t mask = sf ? ~0ull : 0xFFFFFFFFull;
            uint64_t sign = sf ? 0x8000000000000000ull : 0x80000000ull;
            uint64_t n = (res & sign) ? 1 : 0;
            uint64_t z = (res & mask) == 0 ? 1 : 0;
            uint64_t c = sub ? (a >= imm) : (r < a);
            uint64_t v = sub ? (((a & ~imm & ~r) | (~a & imm & r)) & sign) != 0
                             : (((a & imm & ~r) | (~a & ~imm & r)) & sign) != 0;
            nzcv = (n << 31) | (z << 30) | ((uint64_t)c << 29) | (v << 28);
        }
        rd = r;
        pc = next;
        return true;
    }

    // ---- logical immediate
    if ((w & 0x7F800000u) == 0x12000000u) {  // AND/ORR/EOR/ANDS (imm)
        bool sf = bits(w, 31, 1);
        int opc = (int)bits(w, 29, 2);
        int N = (int)bits(w, 22, 1);
        int size = sf ? 64 : 32;
        uint64_t wmask, tmask;
        try {
            decode_bit_masks(N, (int)bits(w, 10, 6), (int)bits(w, 16, 6), size, true, wmask, tmask);
        } catch (...) {
            return false;
        }
        uint64_t a = x[rn];
        uint64_t r;
        if (opc == 0) r = a & wmask;
        else if (opc == 1) r = a | wmask;
        else if (opc == 2) r = a ^ wmask;
        else {
            r = a & wmask;
            uint64_t mask = sf ? ~0ull : 0xFFFFFFFFull;
            uint64_t sign = sf ? 0x8000000000000000ull : 0x80000000ull;
            uint64_t n = (r & sign) ? 1 : 0;
            uint64_t z = (r & mask) == 0 ? 1 : 0;
            uint64_t c = 0, v = 0;
            nzcv = (n << 31) | (z << 30) | (c << 29) | (v << 28);
        }
        rd = r;
        pc = next;
        return true;
    }

    // ---- move wide
    if ((w & 0x7F800000u) == 0x12800000u) {  // MOVN/MOVZ/MOVK
        int opc = (int)bits(w, 29, 2), hw = (int)bits(w, 21, 2);
        uint64_t imm = (uint64_t)bits(w, 5, 16) << (hw * 16);
        if (opc == 0) rd = ~imm;
        else if (opc == 2) rd = imm;
        else if (opc == 3) {
            uint64_t mask = 0xFFFFull << (hw * 16);
            rd = (rd & ~mask) | (imm & mask);
        } else return false;
        pc = next;
        return true;
    }

    // ---- bitfield
    if ((w & 0x7F800000u) == 0x13000000u || (w & 0x7F800000u) == 0x53000000u) {  // SBFM/UBFM
        bool sf = bits(w, 31, 1);
        bool sbfm = (w & 0x7F800000u) == 0x13000000u;
        int N = (int)bits(w, 22, 1);
        int immr = (int)bits(w, 16, 6), imms = (int)bits(w, 10, 6);
        int size = sf ? 64 : 32;
        if (N != (int)sf) return false;
        uint64_t wmask, tmask;
        try {
            decode_bit_masks(N, imms, immr, size, false, wmask, tmask);
        } catch (...) {
            return false;
        }
        uint64_t src = x[rn];
        uint64_t rot = immr ? ror(src, immr, size) : src;
        if (sbfm) {
            uint64_t top = (src >> imms) & 1 ? ~0ull : 0;
            rd = (top & ~tmask) | (rot & wmask);
        } else {
            rd = rot & (wmask & tmask);
        }
        pc = next;
        return true;
    }

    // ---- logical shifted register
    if ((w & 0x7F200000u) == 0x0A000000u) {  // AND/ORR/EOR/ANDS (shifted reg)
        bool sf = bits(w, 31, 1);
        int opc = (int)bits(w, 29, 2), sh = (int)bits(w, 22, 2), amt = (int)bits(w, 10, 6);
        uint64_t b = x[rm];
        if (sh == 0) b <<= amt;
        else if (sh == 1) b >>= amt;
        else if (sh == 2) b = (uint64_t)((int64_t)b >> amt);
        else return false;
        uint64_t a = x[rn];
        uint64_t r;
        if (opc == 0) r = a & b;
        else if (opc == 1) r = a | b;
        else if (opc == 2) r = a ^ b;
        else {
            r = a & b;
            uint64_t mask = sf ? ~0ull : 0xFFFFFFFFull;
            uint64_t sign = sf ? 0x8000000000000000ull : 0x80000000ull;
            uint64_t n = (r & sign) ? 1 : 0;
            uint64_t z = (r & mask) == 0 ? 1 : 0;
            nzcv = (n << 31) | (z << 30);
        }
        rd = r;
        pc = next;
        return true;
    }

    // ---- add/sub shifted register
    if ((w & 0x7F200000u) == 0x0B000000u) {  // ADD/SUB (shifted reg)
        bool sf = bits(w, 31, 1);
        bool sub = bits(w, 30, 1);
        int sh = (int)bits(w, 22, 2), amt = (int)bits(w, 10, 6);
        uint64_t b = x[rm];
        if (sh == 0) b <<= amt;
        else if (sh == 1) b >>= amt;
        else if (sh == 2) b = (uint64_t)((int64_t)b >> amt);
        else return false;
        uint64_t a = x[rn];
        rd = sub ? a - b : a + b;
        pc = next;
        return true;
    }

    // ---- add/sub extended register (UXTW/SXTW etc.)
    if ((w & 0x7FE00000u) == 0x0B200000u) {
        bool sf = bits(w, 31, 1);
        bool sub = bits(w, 30, 1);
        int option = (int)bits(w, 13, 3), amt = (int)bits(w, 10, 3);
        uint64_t b = x[rm];
        if (option == 2) b = (uint64_t)(uint32_t)b;
        else if (option == 3) b = (uint64_t)(uint32_t)b;
        else if (option == 6) b = (uint64_t)(int64_t)(int32_t)b;
        else if (option == 7) b = (uint64_t)(int64_t)(int32_t)b;
        else return false;
        b <<= amt;
        uint64_t a = reg(x, sp, rn);
        rd = sub ? a - b : a + b;
        pc = next;
        return true;
    }

    // ---- MOV (register)
    if ((w & 0x7FE0FFE0u) == 0x2A0003E0u) {  // ORR with zr
        rd = x[rm];
        pc = next;
        return true;
    }

    // ---- branches
    if ((w & 0xFC000000u) == 0x14000000u) {  // B
        pc = pc + (uint64_t)sext(bits(w, 0, 26), 26) * 4;
        return true;
    }
    if ((w & 0xFF000010u) == 0x54000000u) {  // B.cond
        int cond = (int)bits(w, 0, 4);
        bool take;
        uint64_t n = (nzcv >> 31) & 1, z = (nzcv >> 30) & 1, c = (nzcv >> 29) & 1, v = (nzcv >> 28) & 1;
        switch (cond) {
            case 0: take = z; break;
            case 1: take = !z; break;
            case 2: take = c; break;
            case 3: take = !c; break;
            case 4: take = n; break;
            case 5: take = !n; break;
            case 6: take = v; break;
            case 7: take = !v; break;
            case 8: take = c && !z; break;
            case 9: take = !(c && !z); break;
            case 10: take = n == v; break;
            case 11: take = n != v; break;
            case 12: take = !z && n == v; break;
            case 13: take = !(!z && n == v); break;
            default: take = true; break;
        }
        pc = take ? pc + (uint64_t)sext(bits(w, 5, 19), 19) * 4 : next;
        return true;
    }
    if ((w & 0x7E000000u) == 0x34000000u || (w & 0x7E000000u) == 0x35000000u) {  // CBZ/CBNZ
        bool sf = bits(w, 31, 1);
        bool nz = (w & 0x7E000000u) == 0x35000000u;
        int rt = (int)bits(w, 0, 5);
        uint64_t val = sf ? x[rt] : (uint32_t)x[rt];
        bool take = nz ? val != 0 : val == 0;
        pc = take ? pc + (uint64_t)sext(bits(w, 5, 19), 19) * 4 : next;
        return true;
    }
    if ((w & 0xFFFFFC1Fu) == 0xD61F0000u) {  // BR
        pc = x[rn];
        return true;
    }

    // ---- loads
    if ((w & 0xFFC00000u) == 0xF9400000u) {  // LDR (unsigned imm, 64-bit)
        int rt = (int)bits(w, 0, 5);
        uint64_t a = x[rn] + ((uint64_t)bits(w, 10, 12) << 3);
        x[rt] = load(a, 8);
        pc = next;
        return true;
    }
    if ((w & 0xFFC00000u) == 0xB9400000u) {  // LDR (unsigned imm, 32-bit)
        int rt = (int)bits(w, 0, 5);
        uint64_t a = x[rn] + ((uint64_t)bits(w, 10, 12) << 2);
        x[rt] = (uint32_t)load(a, 4);
        pc = next;
        return true;
    }
    if ((w & 0xFFE0FC00u) == 0xF8600800u) {  // LDR (register offset, 64-bit)
        int rt = (int)bits(w, 0, 5);
        uint64_t a = x[rn] + x[rm];
        x[rt] = load(a, 8);
        pc = next;
        return true;
    }
    if ((w & 0xFFE0FC00u) == 0xB8600800u) {  // LDR (register offset, 32-bit)
        int rt = (int)bits(w, 0, 5);
        uint64_t a = x[rn] + x[rm];
        x[rt] = (uint32_t)load(a, 4);
        pc = next;
        return true;
    }
    if ((w & 0xFFC00000u) == 0x39400000u) {  // LDRB (unsigned imm)
        int rt = (int)bits(w, 0, 5);
        uint64_t a = x[rn] + (uint64_t)bits(w, 10, 12);
        x[rt] = load(a, 1);
        pc = next;
        return true;
    }
    if ((w & 0xFFC00000u) == 0x79400000u) {  // LDRH (unsigned imm)
        int rt = (int)bits(w, 0, 5);
        uint64_t a = x[rn] + ((uint64_t)bits(w, 10, 12) << 1);
        x[rt] = load(a, 2);
        pc = next;
        return true;
    }

    // ---- NOP / hints
    if (w == 0xD503201Fu) {  // NOP
        pc = next;
        return true;
    }

    return false;
}

}  // namespace ngen
