// JNI wrapper: the game-code pipeline on Android. generate() runs libnativegen (the C API in
// ngen.h); dlopenTest() loads the compiled game .so and calls a function by name. The compile
// step runs the bundled clang/lld as shared libraries (dlopen) - SELinux on some devices
// forbids exec'ing files in app storage, but dlopen works.
#include <jni.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vector>

#include "ngen.h"

static std::string jstr(JNIEnv *env, jstring s) {
    if (!s) return "";
    const char *c = env->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return r;
}

extern "C" JNIEXPORT jstring JNICALL
Java_io_github_powermoonrecomp_NativeGen_generate(JNIEnv *env, jobject, jstring name, jstring nsoPath,
                                                  jlong base, jstring outdir, jstring hlePath, jstring simd) {
    std::string n = jstr(env, name), p = jstr(env, nsoPath), o = jstr(env, outdir),
                h = jstr(env, hlePath), s = jstr(env, simd);
    int r = ngen_generate(n.c_str(), p.c_str(), (unsigned long long)base, o.c_str(),
                          h.empty() ? "" : h.c_str(), s.empty() ? "asm" : s.c_str(), 0, 0, 0, 0);
    std::string msg = r == 0 ? "ok" : ngen_last_error();
    return env->NewStringUTF(msg.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_io_github_powermoonrecomp_NativeGen_lastError(JNIEnv *env, jobject) {
    return env->NewStringUTF(ngen_last_error());
}

// dlopen a .so and call a function with signature void(Cpu*) - the generated code's shape.
// The caller makes the .so read-only first (Android's linker refuses writable libraries).
extern "C" JNIEXPORT jstring JNICALL
Java_io_github_powermoonrecomp_NativeGen_dlopenTest(JNIEnv *env, jobject, jstring soPath, jstring fnName) {
    std::string p = jstr(env, soPath), fn = jstr(env, fnName);
    void *h = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) return env->NewStringUTF((std::string("dlopen: ") + dlerror()).c_str());
    void (*f)(void *) = (void (*)(void *))dlsym(h, fn.c_str());
    if (!f) {
        dlclose(h);
        return env->NewStringUTF((std::string("loaded, but ") + fn + " not found: " + dlerror()).c_str());
    }
    dlclose(h);
    return env->NewStringUTF("ok");
}

// --- On-device compiler via dlopen ---------------------------------------------
// The bundled clang/lld are shared libraries (libclang_main.so, liblld_main.so) that export
// clang_main()/lld_main() plus *_output() returning the captured stdout+stderr. Load them
// once; the app passes the directory they live in (app storage, extracted from assets).

static void *g_clang_handle = nullptr;
static void *g_lld_handle = nullptr;

static std::string loadCompilerLibs(const std::string &libDir) {
    if (g_clang_handle) return "";
    auto open = [&](const char *name) -> void * {
        void *h = dlopen((libDir + "/" + name).c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (!h) fprintf(stderr, "dlopen %s: %s\n", name, dlerror());
        return h;
    };
    // Order matters: libc++ first, then libclang-cpp (all of clang), then the wrappers.
    open("libc++_shared.so");
    open("libclang-cpp.so");
    g_clang_handle = open("libclang_main.so");
    if (!g_clang_handle) return "dlopen libclang_main.so failed";
    g_lld_handle = open("liblld_main.so");
    if (!g_lld_handle) return "dlopen liblld_main.so failed";
    return "";
}

static jstring runCompiler(JNIEnv *env, jstring libDir, jobjectArray args, const char *sym) {
    std::string dir = jstr(env, libDir);
    std::string err = loadCompilerLibs(dir);
    if (!err.empty()) return env->NewStringUTF(err.c_str());
    void *h = strcmp(sym, "clang_main") == 0 ? g_clang_handle : g_lld_handle;
    auto fn = (int (*)(int, const char **))dlsym(h, sym);
    if (!fn) return env->NewStringUTF((std::string(sym) + " not found").c_str());
    auto outfn = (const char *(*)())dlsym(h, (std::string(sym) + "_output").c_str());

    jsize n = env->GetArrayLength(args);
    std::vector<std::string> strs;
    std::vector<const char *> argv;
    strs.reserve(n);
    argv.reserve(n);
    for (jsize i = 0; i < n; i++) {
        jstring s = (jstring)env->GetObjectArrayElement(args, i);
        strs.push_back(jstr(env, s));
        argv.push_back(strs.back().c_str());
    }
    int rc = fn((int)argv.size(), argv.data());
    std::string out = outfn ? outfn() : "";
    return env->NewStringUTF((out + "\n[exit " + std::to_string(rc) + "]").c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_io_github_powermoonrecomp_NativeGen_clangMain(JNIEnv *env, jobject, jstring libDir, jobjectArray args) {
    return runCompiler(env, libDir, args, "clang_main");
}

extern "C" JNIEXPORT jstring JNICALL
Java_io_github_powermoonrecomp_NativeGen_lldMain(JNIEnv *env, jobject, jstring libDir, jobjectArray args) {
    return runCompiler(env, libDir, args, "lld_main");
}
