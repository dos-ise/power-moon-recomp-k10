// C API for the native game-code generator (tools/nativegen), for embedding it as a library
// (Android, ...). The CLI (gen_main.cpp) is a thin wrapper over the same run_gen().
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Generate C for one NSO module into outdir. Returns 0 on success, -1 on error (ngen_last_error
// then says why). name: module name (main / subsdk0 / sdk); nso_path: the NSO file; base: its
// load address; outdir: where the .c files go; hle_path: hle_symbols.txt ("" = none); simd:
// "c" (portable C) or "asm" (AArch64 inline asm, for AArch64 hosts). chunk / chunk_bytes /
// big_fn / limit: the chunking knobs (0 = defaults).
int ngen_generate(const char *name, const char *nso_path, unsigned long long base,
                  const char *outdir, const char *hle_path, const char *simd,
                  int chunk, int chunk_bytes, int big_fn, int limit);

// The last error message ("" if none). Valid until the next ngen_generate call.
const char *ngen_last_error(void);

#ifdef __cplusplus
}
#endif
