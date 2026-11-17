// lld_main.cpp - expose lld's ELF linker as a callable function for dlopen.
//
// Same rationale as clang_main.cpp: the app cannot exec files in its data
// directory on some devices, so the linker is shipped as a shared library
// (liblld_main.so) that the app dlopens and calls. Only the ELF flavor is
// linked in (the game code is aarch64-linux-android ELF).
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "lld/ELF/Driver.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

static std::string g_output;

extern "C" const char *lld_main_output() { return g_output.c_str(); }

extern "C" int lld_main(int argc, const char **argv) {
    std::string buf;
    llvm::raw_string_ostream os(buf);
    std::vector<const char *> args(argv, argv + argc);
    bool ok = lld::elf::link(args, os, os, false, false);
    os.flush();
    g_output = os.str();
    return ok ? 0 : 1;
}
