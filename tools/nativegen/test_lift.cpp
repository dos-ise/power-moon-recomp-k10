// Decode every word of the text section with the native lifter (both SIMD modes) and report
// coverage. This validates the port against the real game data.
#include <cstdio>
#include <map>
#include <string>

#include "lifter.h"
#include "nso.h"

using namespace ngen;

static void run(const char *path, const char *simd) {
    Nso n(path);
    uint64_t lo = n.text.first, hi = n.text.first + n.text.second;
    Lifter lf(0x7000000000ull, simd);
    uint64_t total = 0, unsupported = 0;
    std::map<std::string, int> uns;
    for (uint64_t a = lo; a < hi; a += 4) {
        uint32_t w;
        memcpy(&w, n.image.data() + a, 4);
        total++;
        try {
            lf.lift(w, a);
        } catch (const Unsupported &e) {
            unsupported++;
            uns[e.what()]++;
        }
    }
    printf("%s [%s]: %llu words, %llu unsupported (%.2f%%)\n", path, simd,
           (unsigned long long)total, (unsigned long long)unsupported,
           total ? 100.0 * (double)unsupported / (double)total : 0.0);
    int shown = 0;
    for (auto &kv : uns) {
        if (shown++ >= 12) break;
        printf("    %-28s %d\n", kv.first.c_str(), kv.second);
    }
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        run(argv[i], "c");
        run(argv[i], "asm");
    }
    return 0;
}
