// Generator driver: function discovery, jump-table resolution, C emission, chunking.
// Port of tools/gen.py (Module class + main()).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "interp.h"
#include "lifter.h"
#include "nso.h"

namespace ngen {

struct Gen;

// A per-instruction replacement (tools/odyssey_patches.py): returns C code for the instruction
// at guest address `a`.
using PatchFn = std::function<std::string(Gen &, uint64_t)>;

struct ScanResult {
    std::set<uint64_t> labels;
    std::set<uint64_t> ext;
    std::map<uint64_t, std::vector<uint64_t>> tables;  // br_pc -> targets, or empty = 'all'
    bool all_labels = false;  // unresolved table: allow jumping to any instruction
};

class Gen {
public:
    Gen(const std::string &name, const std::string &path, uint64_t base, const std::string &simd);

    std::string name;
    Nso nso;
    uint64_t base;
    uint64_t lo, hi;
    std::vector<uint32_t> words;
    Lifter lifter;
    std::map<uint64_t, PatchFn> patches;  // by build (odyssey_patches)
    std::map<uint64_t, std::string> hle;  // address -> hle symbol name
    std::map<uint64_t, std::string> hle_extra;  // build-specific hle by address

    bool in_text(uint64_t a) const;
    uint32_t word(uint64_t a) const;
    Insn lift(uint64_t a);

    // ---- function discovery
    std::vector<uint64_t> boundaries();

    // ---- jump tables
    std::vector<uint64_t> jump_table(uint64_t br_pc, uint64_t entry, uint64_t end);
    bool computed_target(uint64_t br_pc, uint64_t entry);
    ScanResult scan(uint64_t entry, uint64_t end);

    // ---- emission
    std::string render_single(const Insn &ins, uint64_t a);
    std::vector<std::string> emit_function(uint64_t entry, uint64_t end,
                                           const std::set<uint64_t> &labels,
                                           const std::map<uint64_t, std::vector<uint64_t>> &tables,
                                           const std::map<uint64_t, std::string> &hle,
                                           std::set<uint64_t> &refs);

    // stats
    std::map<std::string, int> stats;

private:
    Interp *emu();
    std::vector<uint64_t> _jump_table_fallthrough(uint64_t br_pc, uint64_t entry, uint64_t end);
    std::vector<uint64_t> _jump_table_taken(uint64_t br_pc, uint64_t entry, uint64_t end);
    std::vector<int> _index_regs(uint64_t start, uint64_t at, int idx);
    void _set_index(const std::vector<int> &regs, uint64_t i);
    std::vector<uint64_t> _emulate_table(uint64_t br_pc, uint64_t entry, uint64_t end, uint64_t start,
                                         int idx, int count, uint64_t set_at);
    Interp *mu_ = nullptr;
};

// odyssey_patches: build -> patches (see patches.h)
// gen.py main(): generate C for one module.
int run_gen(const std::string &name, const std::string &nso_path, uint64_t base,
            const std::string &outdir, const std::string &hle_path, const std::string &simd,
            int chunk, int chunk_bytes, int big_fn, int limit);

}  // namespace ngen
