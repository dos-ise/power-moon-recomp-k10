// JNI wrapper: the game-code pipeline on Android. generate() runs libnativegen (the C API in
// ngen.h); dlopenTest() loads the compiled game .so and calls a function by name. The compile
// step runs the bundled clang via ProcessBuilder (Kotlin), not here.
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
