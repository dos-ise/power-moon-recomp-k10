// AArch64 -> C static recompiler (port of tools/lifter.py).
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "arm64.h"

namespace ngen {

class SimdC;

// An FP/SIMD instruction to pass through as inline asm.
struct AsmInsn {
    uint32_t word = 0;
    std::set<int> vregs;                              // V register numbers touched
    std::vector<std::pair<int, std::string>> gprs;    // (bit position, guest reg or "sp")
    bool flags = false;
    std::vector<std::pair<int, uint64_t>> consts;     // (bit position, constant)
};

// Result of lifting one instruction.
struct Insn {
    enum Kind { Op, Asm, B, Bcond, Bl, Blr, Br, Ret, Trap } kind = Op;
    std::string code;        // C statements (Op), or condition expr (Bcond)
    uint64_t target = 0;     // numeric branch target (B, Bcond, Bl)
    std::string target_expr; // register expr (Br, Blr)
    AsmInsn asm_insn;
};

class Unsupported : public std::exception {
public:
    explicit Unsupported(std::string m) : msg_(std::move(m)) {}
    const char *what() const noexcept override { return msg_.c_str(); }

private:
    std::string msg_;
};

class Lifter {
public:
    Lifter(uint64_t base, const std::string &simd);

    uint64_t base;
    SimdC *sc = nullptr;  // non-null when simd == 'c'

    Insn lift(uint32_t w, uint64_t pc);

    // emit_asm_batch: one inline-asm block for a run of FP/SIMD instructions.
    static std::string emit_asm_batch(const std::vector<AsmInsn> &batch);

private:
    Insn dp_imm(uint32_t w, uint64_t pc);
    Insn branch_sys(uint32_t w, uint64_t pc);
    Insn system(uint32_t w, uint64_t pc);
    Insn ldst(uint32_t w, uint64_t pc);
    Insn ldst_excl(uint32_t w, uint64_t pc);
    Insn ldst_pair(uint32_t w);
    Insn ldst_reg(uint32_t w);
    Insn ldst_simd(uint32_t w, uint64_t pc);
    Insn dp_reg(uint32_t w, uint64_t pc);
    Insn fp_simd(uint32_t w, uint64_t pc);
};

}  // namespace ngen
