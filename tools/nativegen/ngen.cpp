// C API implementation: ngen_generate() -> ngen::run_gen(), with the last error kept for
// ngen_last_error(). The generator is pure C++17 and portable (dirent.h, sys/stat.h, fstream),
// so this builds for Android with the NDK as well as for the desktop hosts.
#include "ngen.h"

#include <exception>
#include <string>

#include "gen.h"

namespace {
std::string g_err;
}

extern "C" int ngen_generate(const char *name, const char *nso_path, unsigned long long base,
                             const char *outdir, const char *hle_path, const char *simd,
                             int chunk, int chunk_bytes, int big_fn, int limit) {
    g_err.clear();
    try {
        return ngen::run_gen(name ? name : "", nso_path ? nso_path : "", base,
                             outdir ? outdir : "", hle_path ? hle_path : "", simd ? simd : "c",
                             chunk, chunk_bytes, big_fn, limit);
    } catch (const std::exception &e) {
        g_err = e.what();
        return -1;
    } catch (...) {
        g_err = "unknown error";
        return -1;
    }
}

extern "C" const char *ngen_last_error(void) { return g_err.c_str(); }
