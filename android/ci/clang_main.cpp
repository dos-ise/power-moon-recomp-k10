// clang_main.cpp - expose clang's driver as a callable function for dlopen.
//
// Android's SELinux policy on some devices (e.g. Samsung) forbids an app from
// exec'ing files in its own data directory (app_data_file), but dlopen works.
// So instead of shipping the clang executable, this wrapper is linked into a
// shared library (libclang_main.so) that the app dlopens and calls directly.
// It mirrors clang/tools/driver/driver.cpp's main(), minus the process setup.
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

static std::string g_output;

extern "C" const char *clang_main_output() { return g_output.c_str(); }

extern "C" int clang_main(int argc, const char **argv) {
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();

    std::string buf;
    llvm::raw_string_ostream os(buf);

    std::string path = argv[0];
    llvm::IntrusiveRefCntPtr<clang::DiagnosticOptions> DiagOpts = new clang::DiagnosticOptions();
    clang::TextDiagnosticPrinter *DiagClient = new clang::TextDiagnosticPrinter(os, &*DiagOpts);
    llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs> DiagID = new clang::DiagnosticIDs();
    clang::DiagnosticsEngine Diags(DiagID, &*DiagOpts, DiagClient);

    clang::driver::Driver TheDriver(path, llvm::sys::getDefaultTargetTriple(), Diags);
    TheDriver.setCheckInputsExist(false);

    std::vector<const char *> args(argv, argv + argc);
    std::unique_ptr<clang::driver::Compilation> C(TheDriver.BuildCompilation(args));
    int rc = 1;
    if (C) {
        llvm::SmallVector<std::pair<int, const clang::driver::Command *>, 4> FailingCommands;
        rc = TheDriver.ExecuteCompilation(*C, FailingCommands);
    }
    os.flush();
    g_output = os.str();
    return rc;
}
