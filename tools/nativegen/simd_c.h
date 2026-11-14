// Portable C lowering of AArch64 FP / AdvSIMD / crypto / CRC32 (port of tools/simd_c.py).
// Every encoding is decoded at lift time and turned into C over the Cpu struct, calling the
// static inline helpers in runtime/simd.h with constant fields.
#pragma once

#include <cstdint>
#include <string>

namespace ngen {

class SimdC {
public:
    // Top-level FP/SIMD data-processing decode; returns a C statement or throws Unsupported.
    std::string dp(uint32_t w, uint64_t pc);
    // FP/SIMD load/store; returns a C statement or throws Unsupported.
    std::string ldst(uint32_t w, uint64_t pc);
    // CRC32 (data processing register); returns a C statement.
    std::string crc(uint32_t w);

private:
    std::string crypto_aes(uint32_t w);
    std::string crypto_sha3(uint32_t w);
    std::string crypto_sha2(uint32_t w);
    std::string scalar_copy(uint32_t w);
    std::string copy(uint32_t w);
    std::string permute(uint32_t w);
    std::string ext(uint32_t w);
    std::string tbl(uint32_t w);
    std::string three_same(uint32_t w, bool scalar);
    std::string three_same_scalar(int U, int sz, int opc, int d, int n, int m);
    std::string three_same_fp(uint32_t w, bool scalar);
    std::string three_diff(uint32_t w, bool scalar);
    std::string misc2(uint32_t w, bool scalar);
    std::string misc2_scalar(int U, int sz, int opc, int d, int n);
    std::string misc2_fp(uint32_t w, bool scalar);
    std::string across(uint32_t w);
    std::string scalar_pairwise(uint32_t w);
    std::string modimm(uint32_t w);
    std::string shift_imm(uint32_t w, bool scalar);
    std::string indexed(uint32_t w, bool scalar);
    std::string fp_dp3(uint32_t w);
    std::string fp_dp2(uint32_t w);
    std::string fp_dp1(uint32_t w);
    std::string fp_cmp(uint32_t w);
    std::string fp_ccmp(uint32_t w);
    std::string fp_csel(uint32_t w);
    std::string fp_imm(uint32_t w);
    std::string fp_fixed(uint32_t w);
    std::string fp_int(uint32_t w);
    std::string ldst_pair(uint32_t w);
    std::string ldst_reg(uint32_t w);
    std::string ldst_struct(uint32_t w);
};

}  // namespace ngen
