// NSO loader: decompress segments, build the memory image, parse MOD0/dynamic info.
// Native port of tools/nso.py (no external dependencies; LZ4 block decompression is
// implemented here so the generator runs anywhere, including Android).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ngen {

// LZ4 block decompression (raw block format, as lz4.block.decompress with a known size).
// Returns false if the input is malformed or the output would exceed `out_size`.
bool lz4_block_decompress(const uint8_t *src, size_t src_size, uint8_t *dst, size_t out_size);

struct Symbol {
    std::string name;
    uint8_t info;
    uint8_t other;
    uint16_t shndx;
    uint64_t value;
    uint64_t size;
};

struct Reloc {
    uint64_t off;
    uint32_t type;
    uint32_t sym;
    int64_t add;
};

// Dynamic tags used by the loader.
enum : uint64_t {
    DT_NULL = 0, DT_NEEDED = 1, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5,
    DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8, DT_STRSZ = 10, DT_JMPREL = 23,
    DT_RELACOUNT = 0x6FFFFFF9,
};
enum : uint32_t {
    R_AARCH64_ABS64 = 257, R_AARCH64_GLOB_DAT = 1025, R_AARCH64_JUMP_SLOT = 1026,
    R_AARCH64_RELATIVE = 1027,
};

class Nso {
public:
    explicit Nso(const std::string &path);

    std::string build_id;          // hex, 32 chars (16 bytes)
    uint32_t bss_size = 0;
    std::pair<uint64_t, uint64_t> text;    // (mem offset, size)
    std::pair<uint64_t, uint64_t> rodata;  // (mem offset, size)
    std::pair<uint64_t, uint64_t> data;    // (mem offset, size)
    uint64_t size = 0;             // image size (page aligned)
    std::vector<uint8_t> image;    // the memory image

    uint64_t dynamic = 0, bss_start = 0, bss_end = 0;
    uint64_t eh_start = 0, eh_end = 0, module_obj = 0;
    uint64_t strtab = 0;
    std::vector<std::string> needed;
    std::vector<Symbol> syms;
    std::vector<Reloc> relocs;

    uint32_t u32(uint64_t off) const;
    uint64_t u64(uint64_t off) const;
    std::string cstr(uint64_t off) const;

    std::vector<Symbol> imports() const;
    std::vector<Symbol> exports() const;

private:
    void _parse_mod0();
};

}  // namespace ngen
