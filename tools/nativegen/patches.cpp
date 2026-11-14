// Per-instruction replacements for Super Mario Odyssey (port of tools/odyssey_patches.py).
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>

#include "gen.h"
#include "lifter.h"

namespace ngen {

namespace {

const char *ODYSSEY_MAIN_BUILD = "3ca12dfaaf9c82da";
const char *ODYSSEY_141_BUILD = "b424be150a8e7d78";

std::string hx_addr(uint64_t a) {
    char buf[24];
    snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)a);
    return buf;
}
std::string hex_no_prefix(uint64_t a) {
    char buf[24];
    snprintf(buf, sizeof buf, "%llx", (unsigned long long)a);
    return buf;
}

uint32_t read_word(Gen &g, uint64_t a) {
    uint32_t w;
    memcpy(&w, g.nso.image.data() + (a - g.base), 4);
    return w;
}

// sead::Heap::Heap + 0xe8: csel w8, w8, w9, ne -> heap lock always on
std::string heap_lock(Gen &g, uint64_t a) {
    uint32_t w = read_word(g, a);
    if (w != 0x1a891108u) throw Unsupported("heap lock word");
    return "c->x[8] = (uint32_t)c->x[8];  /* " + hx_addr(a) +
           ": csel w8, w8, w9, ne -> heap lock always on */";
}

struct WideSite {
    uint32_t word;
    int reg;
    const char *k;
};

// 1.0.0 wide-screen sites
const std::map<uint64_t, WideSite> WIDE_SITES = {
    {0x7000837914ull, {0x2d580663u, 3, "odyssey_wide()"}},
    {0x7000837a54ull, {0x2d578e61u, 3, "odyssey_wide_camera()"}},
    {0x7000a63c94ull, {0x1e230000u, 0, "odyssey_wide_drawinfo(c->x[20])"}},
    {0x7000a63cb4ull, {0x1e230000u, 0, "odyssey_wide()"}},
    {0x70001e813cull, {0x1e230000u, 0, "odyssey_wide()"}},
    {0x70001e816cull, {0x1e230001u, 1, "odyssey_wide()"}},
    {0x70003fb8b0ull, {0x1e230000u, 0, "odyssey_wide()"}},
    {0x7000883064ull, {0x1e230008u, 8, "odyssey_wide()"}},
};

std::string wide(Gen &g, uint64_t a, const WideSite &ws) {
    uint32_t w = read_word(g, a);
    if (w != ws.word) throw Unsupported("wide word");
    std::string code = g.render_single(g.lift(a), a);
    return code + " { extern float odyssey_wide(void), odyssey_wide_camera(void), "
                  "odyssey_wide_drawinfo(uint64_t); FP_SETS(" +
           std::to_string(ws.reg) + ", VSF32(" + std::to_string(ws.reg) + ") * " + ws.k + "); }  /* " +
           hx_addr(a) + ": wide screen */";
}

std::string debug_hook(Gen &g, uint64_t a) {
    Insn ins = g.lift(a);
    if (ins.kind != Insn::Op && ins.kind != Insn::Asm && ins.kind != Insn::Trap && ins.kind != Insn::Ret)
        throw Unsupported("no hook on a branch");
    std::string code = g.render_single(ins, a);
    return "{ extern void odyssey_debug_hook(Cpu *, uint64_t); odyssey_debug_hook(c, " + hx_addr(a) +
           "ull); } " + code;
}

std::string particle_tick(Gen &g, uint64_t a, uint64_t target) {
    Insn ins = g.lift(a);
    if (ins.kind != Insn::Bl || ins.target != target) throw Unsupported("unexpected particle update call");
    std::string code = g.render_single(ins, a);
    return "{ extern _Bool odyssey_skip_extra_particle_update(void); extern void f_" +
           hex_no_prefix(target) + "(Cpu *); if (!odyssey_skip_extra_particle_update()) { " + code + " } }";
}

// 1.0.0 patches
std::map<uint64_t, PatchFn> make_patches_100() {
    std::map<uint64_t, PatchFn> p;
    p[0x7000743fa0ull] = [](Gen &g, uint64_t a) { return heap_lock(g, a); };
    for (auto &kv : WIDE_SITES) {
        const WideSite ws = kv.second;
        p[kv.first] = [ws](Gen &g, uint64_t a) { return wide(g, a, ws); };
    }
    p[0x700094c428ull] = [](Gen &g, uint64_t a) { return debug_hook(g, a); };
    p[0x70004ccbc8ull] = [](Gen &g, uint64_t a) { return debug_hook(g, a); };
    p[0x7000b398d0ull] = [](Gen &g, uint64_t a) { return particle_tick(g, a, 0x7000b45ae8ull); };
    p[0x7000b39d2cull] = [](Gen &g, uint64_t a) { return particle_tick(g, a, 0x7000b45ae8ull); };
    return p;
}

// 1.4.1 sites (addresses found by tools/port141.py)
const std::map<uint64_t, uint64_t> SITES_141 = {
    {0x7000743fa0ull, 0x7000954ea4ull},
    {0x7000837914ull, 0x70005adf40ull},
    {0x7000837a54ull, 0x70005ae094ull},
    {0x7000a63c94ull, 0x70006330d4ull},
    {0x7000a63cb4ull, 0x70006330fcull},
    {0x70001e813cull, 0x70001ca548ull},
    {0x70001e816cull, 0x70001ca57cull},
    {0x70003fb8b0ull, 0x70003a1008ull},
    {0x7000883064ull, 0x70005fcdc4ull},
    {0x700094c428ull, 0x70006b9ec0ull},
    {0x70004ccbc8ull, 0x7000473354ull},
    {0x7000b398d0ull, 0x700091e0acull},
    {0x7000b39d2cull, 0x700091e2a4ull},
};
const uint64_t CALC_COMPUTE_SHADER_141 = 0x700092c020ull;

std::map<uint64_t, PatchFn> make_patches_141() {
    std::map<uint64_t, PatchFn> p;
    p[SITES_141.at(0x7000743fa0ull)] = [](Gen &g, uint64_t a) { return heap_lock(g, a); };
    for (auto &kv : WIDE_SITES) {
        uint64_t a141 = SITES_141.at(kv.first);
        WideSite ws = kv.second;
        if (kv.first == 0x7000a63c94ull) ws.k = "odyssey_wide_drawinfo(c->x[19])";
        p[a141] = [ws](Gen &g, uint64_t a) { return wide(g, a, ws); };
    }
    p[SITES_141.at(0x700094c428ull)] = [](Gen &g, uint64_t a) { return debug_hook(g, a); };
    p[SITES_141.at(0x70004ccbc8ull)] = [](Gen &g, uint64_t a) { return debug_hook(g, a); };
    p[SITES_141.at(0x7000b398d0ull)] = [](Gen &g, uint64_t a) {
        return particle_tick(g, a, CALC_COMPUTE_SHADER_141);
    };
    p[SITES_141.at(0x7000b39d2cull)] = [](Gen &g, uint64_t a) {
        return particle_tick(g, a, CALC_COMPUTE_SHADER_141);
    };
    return p;
}

}  // namespace

const std::map<std::string, std::map<uint64_t, PatchFn>> &patches_by_build() {
    static const std::map<std::string, std::map<uint64_t, PatchFn>> m = {
        {ODYSSEY_MAIN_BUILD, make_patches_100()},
        {ODYSSEY_141_BUILD, make_patches_141()},
    };
    return m;
}

const std::map<std::string, std::map<uint64_t, std::string>> &hle_by_build() {
    static const std::map<std::string, std::map<uint64_t, std::string>> m = {
        {ODYSSEY_141_BUILD, {{0x700097bfa0ull, "_ZN4sead6system4HaltEv"}}},
    };
    return m;
}

}  // namespace ngen
