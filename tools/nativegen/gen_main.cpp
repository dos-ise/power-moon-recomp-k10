// Command-line driver for the native generator (same interface as tools/gen.py).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "gen.h"

using namespace ngen;

static void usage() {
    fprintf(stderr,
            "usage: gen <name> <nso> <base> <outdir> [--hle hle_symbols.txt] [--chunk N] "
            "[--chunk-bytes N] [--big-fn N] [--limit N] [--simd asm|c]\n");
}

int main(int argc, char **argv) {
    if (argc < 5) {
        usage();
        return 2;
    }
    std::string name = argv[1], nso_path = argv[2], outdir = argv[4];
    uint64_t base = (uint64_t)strtoull(argv[3], nullptr, 0);
    std::string hle_path;
#if defined(__aarch64__)
    std::string simd = "asm";  // AArch64 hosts run the guest's FP/SIMD as inline asm (as gen.py)
#else
    std::string simd = "c";    // portable C elsewhere
#endif
    int chunk = 1500, chunk_bytes = 1500000, big_fn = 100000, limit = 0;
    for (int i = 5; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value for %s\n", a.c_str());
                exit(2);
            }
            return argv[++i];
        };
        if (a == "--hle") hle_path = next();
        else if (a == "--chunk") chunk = atoi(next().c_str());
        else if (a == "--chunk-bytes") chunk_bytes = atoi(next().c_str());
        else if (a == "--big-fn") big_fn = atoi(next().c_str());
        else if (a == "--limit") limit = atoi(next().c_str());
        else if (a == "--simd") simd = next();
        else {
            fprintf(stderr, "unknown option %s\n", a.c_str());
            usage();
            return 2;
        }
    }
    try {
        return run_gen(name, nso_path, base, outdir, hle_path, simd, chunk, chunk_bytes, big_fn,
                       limit);
    } catch (const std::exception &e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
