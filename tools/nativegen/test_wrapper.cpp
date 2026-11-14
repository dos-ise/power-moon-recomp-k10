// Synthetic test for the odyssey_patches ORIGINAL_WRAPPERS_BY_BUILD port and the co-op camera
// patch: builds a minimal in-memory NSO0 (no game files), runs emit_function on the wrapped
// addresses and checks the emitted C. Mirrors tools/test_original_wrapper.py for the native port.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gen.h"

using namespace ngen;

static void put32(std::vector<uint8_t> &b, size_t off, uint32_t v) {
    b[off] = (uint8_t)v;
    b[off + 1] = (uint8_t)(v >> 8);
    b[off + 2] = (uint8_t)(v >> 16);
    b[off + 3] = (uint8_t)(v >> 24);
}

// A minimal uncompressed NSO0 whose text segment covers the wrapped addresses, with a MOD0 +
// empty dynamic section so the loader is happy. Returns the temp file path.
static std::string write_synthetic_nso(const char *path) {
    const uint32_t TEXT_OFF = 0x100, TEXT_SIZE = 0xC00000;  // text at file 0x100, mem 0 (12MB covers 0xaeee10)
    const uint32_t RODATA_OFF = TEXT_OFF + TEXT_SIZE, RODATA_SIZE = 0x1000;
    const uint32_t DATA_OFF = RODATA_OFF + RODATA_SIZE, DATA_SIZE = 0x1000;
    const uint32_t MOD0 = 0x100, DYN = 0x120;

    std::vector<uint8_t> f(DATA_OFF + DATA_SIZE, 0);
    memcpy(f.data(), "NSO0", 4);
    put32(f, 0x0C, 0);  // flags: all segments uncompressed
    // Segment descriptors are 0x10 apart (0x10/0x20/0x30), bss at 0x3C, build id at 0x40 - the
    // layout tools/nso.py and tools/nativegen/nso.cpp read (matches the real NSO0 files).
    put32(f, 0x10, TEXT_OFF);   put32(f, 0x14, 0);      put32(f, 0x18, TEXT_SIZE);
    put32(f, 0x20, RODATA_OFF); put32(f, 0x24, TEXT_SIZE); put32(f, 0x28, RODATA_SIZE);
    put32(f, 0x30, DATA_OFF);   put32(f, 0x34, TEXT_SIZE + RODATA_SIZE); put32(f, 0x38, DATA_SIZE);
    put32(f, 0x3C, 0);  // bss_size
    // build id: first 8 bytes = 3ca12dfaaf9c82da (the 1.0.0 main build)
    static const uint8_t BID[16] = {0x3c, 0xa1, 0x2d, 0xfa, 0xaf, 0x9c, 0x82, 0xda,
                                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    memcpy(f.data() + 0x40, BID, 16);
    put32(f, 0x60, TEXT_SIZE); put32(f, 0x64, RODATA_SIZE); put32(f, 0x68, DATA_SIZE);

    // The image is assembled from the segments (text at file TEXT_OFF, mem 0), so everything
    // below is written at TEXT_OFF + <image offset>.
    // MOD0 at image offset 0x100 (image[4] = mod0 offset), dynamic at 0x120 (DT_NULL only)
    put32(f, TEXT_OFF + 4, MOD0);
    memcpy(f.data() + TEXT_OFF + MOD0, "MOD0", 4);
    put32(f, TEXT_OFF + MOD0 + 4, (int32_t)(DYN - MOD0));       // dynamic
    put32(f, TEXT_OFF + MOD0 + 8, (int32_t)(0 - MOD0));         // bss_start
    put32(f, TEXT_OFF + MOD0 + 12, (int32_t)(0 - MOD0));        // bss_end
    put32(f, TEXT_OFF + MOD0 + 16, (int32_t)(0 - MOD0));        // eh_start
    put32(f, TEXT_OFF + MOD0 + 20, (int32_t)(0 - MOD0));        // eh_end
    put32(f, TEXT_OFF + MOD0 + 24, (int32_t)(0 - MOD0));        // module_obj
    // dynamic: DT_NULL (tag=0, val=0) - already zeroed

    // a `ret` at the wrapped address and at the coop-camera patch address
    const uint32_t RET = 0xD65F03C0u;
    put32(f, TEXT_OFF + (0x7000aeee10ull - 0x7000000000ull), RET);
    put32(f, TEXT_OFF + (0x700044dfb8ull - 0x7000000000ull), RET);

    FILE *fp = fopen(path, "wb");
    assert(fp);
    fwrite(f.data(), 1, f.size(), fp);
    fclose(fp);
    return path;
}

int main() {
    const char *path = "build/test_wrapper.nso";
    write_synthetic_nso(path);
    fprintf(stderr, "nso written\n");

    try {
        Nso n(path);
        fprintf(stderr, "nso loaded\n");
    } catch (const std::exception &e) {
        fprintf(stderr, "nso load threw: %s\n", e.what());
        return 2;
    }
    Gen g("main", path, 0x7000000000ull, "c");
    fprintf(stderr, "gen constructed\n");
    std::set<uint64_t> refs;

    // 1. a wrapped function keeps its body as f_*_original and routes the entry through the wrapper
    auto lines = g.emit_function(0x7000aeee10ull, 0x7000aeee14ull, {}, {}, {}, refs);
    fprintf(stderr, "wrapped emitted\n");
    assert(lines[0] == "static void f_7000aeee10_original(Cpu *c) {");
    assert(lines.back() == "void f_7000aeee10(Cpu *c) { odyssey_coop_find_glyph(c, f_7000aeee10_original); }");
    bool has_decl = false;
    for (auto &l : lines)
        if (l == "void odyssey_coop_find_glyph(Cpu *, GuestFn);") has_decl = true;
    assert(has_decl);

    // 2. a non-wrapped function is emitted unchanged
    auto plain = g.emit_function(0x7000aeee20ull, 0x7000aeee24ull, {}, {}, {}, refs);
    fprintf(stderr, "plain emitted\n");
    assert(plain[0] == "void f_7000aeee20(Cpu *c) {");
    assert(plain.back() == "}");

    // 3. the co-op camera patch emits the runtime hand-off with the return address
    auto cam = g.emit_function(0x700044dfb8ull, 0x700044dfbcull, {}, {}, {}, refs);
    fprintf(stderr, "camera emitted\n");
    bool has_cam = false;
    for (auto &l : cam)
        if (l.find("odyssey_coop_near_zero(c);") != std::string::npos &&
            l.find("c->x[30] = 0x700044dfbcull;") != std::string::npos)
            has_cam = true;
    assert(has_cam);

    printf("test_wrapper: ok\n");
    return 0;
}
