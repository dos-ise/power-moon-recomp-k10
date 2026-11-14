#include "nso.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace ngen {

// ------------------------------------------------------------------ LZ4 block

bool lz4_block_decompress(const uint8_t *src, size_t src_size, uint8_t *dst, size_t out_size) {
    size_t s = 0, d = 0;
    while (s < src_size) {
        uint8_t token = src[s++];
        size_t lit = token >> 4;
        if (lit == 15) {
            uint8_t b;
            do {
                if (s >= src_size) return false;
                b = src[s++];
                lit += b;
            } while (b == 255);
        }
        if (d + lit > out_size || s + lit > src_size) return false;
        memcpy(dst + d, src + s, lit);
        d += lit;
        s += lit;
        if (s >= src_size) break;  // last sequence has no match
        if (s + 2 > src_size) return false;
        uint16_t off = (uint16_t)(src[s] | (src[s + 1] << 8));
        s += 2;
        if (off == 0 || off > d) return false;
        size_t mlen = token & 15;
        if (mlen == 15) {
            uint8_t b;
            do {
                if (s >= src_size) return false;
                b = src[s++];
                mlen += b;
            } while (b == 255);
        }
        mlen += 4;
        if (d + mlen > out_size) return false;
        // Overlapping copy (LZ4 semantics): byte by byte.
        for (size_t i = 0; i < mlen; i++) dst[d + i] = dst[d + i - off];
        d += mlen;
    }
    return d == out_size;
}

// ------------------------------------------------------------------ NSO

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

Nso::Nso(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> raw((size_t)len);
    if (len > 0 && fread(raw.data(), 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        throw std::runtime_error("short read " + path);
    }
    fclose(f);

    if (raw.size() < 0x100 || memcmp(raw.data(), "NSO0", 4) != 0)
        throw std::runtime_error("not an NSO0: " + path);
    uint32_t flags = rd32(raw.data() + 0xC);
    build_id.resize(32);
    for (int i = 0; i < 16; i++) snprintf(&build_id[2 * i], 3, "%02x", raw[0x40 + i]);
    bss_size = rd32(raw.data() + 0x3C);

    struct Seg { uint32_t file_off, mem_off, dec_size, comp_size; bool compressed; };
    Seg segs[3];
    for (int i = 0; i < 3; i++) {
        segs[i].file_off = rd32(raw.data() + 0x10 + i * 0x10);
        segs[i].mem_off = rd32(raw.data() + 0x14 + i * 0x10);
        segs[i].dec_size = rd32(raw.data() + 0x18 + i * 0x10);
        segs[i].comp_size = rd32(raw.data() + 0x60 + i * 4);
        segs[i].compressed = flags & (1u << i);
    }
    text = {segs[0].mem_off, segs[0].dec_size};
    rodata = {segs[1].mem_off, segs[1].dec_size};
    data = {segs[2].mem_off, segs[2].dec_size};
    uint64_t end = (uint64_t)segs[2].mem_off + segs[2].dec_size + bss_size;
    size = (end + 0xFFF) & ~0xFFFull;
    image.assign(size, 0);
    for (int i = 0; i < 3; i++) {
        const uint8_t *src = raw.data() + segs[i].file_off;
        size_t n = segs[i].comp_size;
        if (segs[i].compressed) {
            if (!lz4_block_decompress(src, n, image.data() + segs[i].mem_off, segs[i].dec_size))
                throw std::runtime_error("LZ4 decompress failed: " + path);
        } else {
            memcpy(image.data() + segs[i].mem_off, src, n);
        }
    }
    _parse_mod0();
}

uint32_t Nso::u32(uint64_t off) const {
    return rd32(image.data() + off);
}
uint64_t Nso::u64(uint64_t off) const {
    const uint8_t *p = image.data() + off;
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

std::string Nso::cstr(uint64_t off) const {
    const uint8_t *p = image.data() + off;
    const uint8_t *e = (const uint8_t *)memchr(p, 0, image.size() - off);
    if (!e) e = image.data() + image.size();
    std::string s((const char *)p, (size_t)(e - p));
    // decode utf-8 with replacement (Python's decode('utf-8','replace'))
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { out += (char)c; continue; }
        int need = 0;
        if ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else { out += "\xEF\xBF\xBD"; continue; }
        if (i + need >= s.size()) { out += "\xEF\xBF\xBD"; continue; }
        bool ok = true;
        for (int k = 1; k <= need; k++)
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) ok = false;
        if (!ok) { out += "\xEF\xBF\xBD"; continue; }
        out.append(s, i, (size_t)need + 1);
        i += need;
    }
    return out;
}

void Nso::_parse_mod0() {
    uint32_t mod0 = u32(4);
    if (mod0 + 4 > image.size() || memcmp(image.data() + mod0, "MOD0", 4) != 0)
        throw std::runtime_error("no MOD0");
    auto rel = [&](int i) -> uint64_t {
        int32_t d;
        memcpy(&d, image.data() + mod0 + 4 + i * 4, 4);
        return (uint64_t)((int64_t)mod0 + d);
    };
    dynamic = rel(0);
    bss_start = rel(1);
    bss_end = rel(2);
    eh_start = rel(3);
    eh_end = rel(4);
    module_obj = rel(5);

    uint64_t off = dynamic;
    std::vector<std::pair<uint64_t, uint64_t>> dyn;
    std::vector<uint64_t> needed_off;
    while (true) {
        uint64_t tag = u64(off), val = u64(off + 8);
        off += 16;
        if (tag == DT_NULL) break;
        if (tag == DT_NEEDED) needed_off.push_back(val);
        dyn.emplace_back(tag, val);
    }
    auto get = [&](uint64_t tag) -> uint64_t {
        for (auto &kv : dyn)
            if (kv.first == tag) return kv.second;
        return 0;
    };
    strtab = get(DT_STRTAB);
    for (uint64_t n : needed_off) needed.push_back(cstr(strtab + n));
    uint64_t hash = get(DT_HASH), symtab = get(DT_SYMTAB);
    if (hash && symtab) {
        uint32_t nchain = u32(hash + 4);
        for (uint32_t i = 0; i < nchain; i++) {
            uint64_t o = symtab + (uint64_t)i * 24;
            Symbol s;
            uint32_t name = u32(o);
            s.info = image[o + 4];
            s.other = image[o + 5];
            uint16_t shndx;
            memcpy(&shndx, image.data() + o + 6, 2);
            s.shndx = shndx;
            s.value = u64(o + 8);
            s.size = u64(o + 16);
            s.name = cstr(strtab + name);
            syms.push_back(std::move(s));
        }
    }
    for (uint64_t base_tag : {DT_RELA, DT_JMPREL}) {
        uint64_t b = get(base_tag);
        if (!b) continue;
        uint64_t n = get(base_tag == DT_RELA ? DT_RELASZ : DT_PLTRELSZ) / 24;
        for (uint64_t i = 0; i < n; i++) {
            Reloc r;
            r.off = u64(b + i * 24);
            uint64_t info = u64(b + i * 24 + 8);
            int64_t add;
            memcpy(&add, image.data() + b + i * 24 + 16, 8);
            r.type = (uint32_t)(info & 0xFFFFFFFF);
            r.sym = (uint32_t)(info >> 32);
            r.add = add;
            relocs.push_back(r);
        }
    }
}

std::vector<Symbol> Nso::imports() const {
    std::vector<Symbol> out;
    for (auto &s : syms)
        if (!s.name.empty() && s.shndx == 0) out.push_back(s);
    return out;
}
std::vector<Symbol> Nso::exports() const {
    std::vector<Symbol> out;
    for (auto &s : syms)
        if (!s.name.empty() && s.shndx != 0) out.push_back(s);
    return out;
}

}  // namespace ngen
