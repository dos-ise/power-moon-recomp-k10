#include "ehframe.h"

#include <cstring>
#include <stdexcept>

namespace ngen {

namespace {

// DWARF pointer encoding (read_encoded in analyze.py).
struct Encoded {
    int64_t v;
    size_t size;
};

Encoded read_encoded(const std::vector<uint8_t> &img, uint64_t off, uint8_t enc, uint64_t pc_base) {
    uint8_t fmt = enc & 0x0F;
    uint8_t app = enc & 0x70;
    int64_t v = 0;
    size_t size = 0;
    const uint8_t *p = img.data() + off;
    if (fmt == 0x0B) {  // sdata4
        int32_t x;
        memcpy(&x, p, 4);
        v = x;
        size = 4;
    } else if (fmt == 0x03) {  // udata4
        uint32_t x;
        memcpy(&x, p, 4);
        v = x;
        size = 4;
    } else if (fmt == 0x0C) {  // sdata8
        int64_t x;
        memcpy(&x, p, 8);
        v = x;
        size = 8;
    } else if (fmt == 0x04) {  // udata8
        uint64_t x;
        memcpy(&x, p, 8);
        v = (int64_t)x;
        size = 8;
    } else {
        throw std::runtime_error("unsupported encoding");
    }
    if (app == 0x10) v += (int64_t)off;
    else if (app == 0x30) v += (int64_t)pc_base;
    return {v, size};
}

}  // namespace

std::vector<uint64_t> eh_frame_functions(const Nso &n) {
    std::vector<uint64_t> out;
    uint64_t hdr = n.eh_start;
    if (!hdr || hdr >= n.image.size()) return out;
    uint8_t ver = n.image[hdr], ptr_enc = n.image[hdr + 1], cnt_enc = n.image[hdr + 2],
            tbl_enc = n.image[hdr + 3];
    (void)ver;
    uint64_t off = hdr + 4;
    Encoded e = read_encoded(n.image, off, ptr_enc, hdr);
    off += e.size;
    e = read_encoded(n.image, off, cnt_enc, hdr);
    off += e.size;
    uint64_t count = (uint64_t)e.v;
    for (uint64_t i = 0; i < count; i++) {
        e = read_encoded(n.image, off, tbl_enc, hdr);
        off += e.size;
        out.push_back((uint64_t)e.v);
        e = read_encoded(n.image, off, tbl_enc, hdr);
        off += e.size;
    }
    return out;
}

}  // namespace ngen
