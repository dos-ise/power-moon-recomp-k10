// Minimal AArch64 interpreter for jump-table resolution (replaces Unicorn in gen.py).
// Conservative: any instruction it does not know returns false, and the caller falls back to
// the all-labels mode (which gen.py already handles), so the output stays correct.
#pragma once

#include <cstdint>
#include <vector>

namespace ngen {

class Interp {
public:
    Interp(const std::vector<uint8_t> &image, uint64_t base);

    // Execute one instruction at *pc (absolute guest address); advances *pc. Returns false if
    // the instruction is not supported (state may be partially modified; caller abandons it).
    bool step(uint64_t &pc);

    uint64_t x[32] = {};
    uint64_t sp = 0;
    uint64_t nzcv = 0;

private:
    const std::vector<uint8_t> &image_;
    uint64_t base_;
    uint32_t word(uint64_t a) const;
    uint64_t load(uint64_t a, int size) const;
};

}  // namespace ngen
