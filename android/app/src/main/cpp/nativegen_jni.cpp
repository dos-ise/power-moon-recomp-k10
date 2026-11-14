// JNI wrapper: the game-code pipeline on Android. generate() runs libnativegen (the C API in
// ngen.h); runCommand() runs the bundled zig to compile the generated C; dlopenTest() loads the
// resulting .so and calls a function by name.
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

// Run a command (e.g. the bundled zig) and return its combined output. argv[0] is the program.
extern "C" JNIEXPORT jstring JNICALL
Java_io_github_powermoonrecomp_NativeGen_runCommand(JNIEnv *env, jobject, jobjectArray argv) {
    std::vector<std::string> args;
    std::vector<char *> cargs;
    jsize n = argv ? env->GetArrayLength(argv) : 0;
    for (jsize i = 0; i < n; i++) {
        jstring s = (jstring)env->GetObjectArrayElement(argv, i);
        args.push_back(jstr(env, s));
    }
    for (auto &a : args) cargs.push_back(const_cast<char *>(a.c_str()));
    cargs.push_back(nullptr);

    std::string out;
    FILE *f = popen(cargs[0] ? cargs[0] : "", "r");
    if (!f) return env->NewStringUTF("popen failed");
    char buf[4096];
    size_t k;
    while ((k = fread(buf, 1, sizeof buf - 1, f)) > 0) {
        buf[k] = 0;
        out += buf;
    }
    int rc = pclose(f);
    out += "\n[exit " + std::to_string(rc) + "]";
    return env->NewStringUTF(out.c_str());
}

// dlopen a .so and call a function with signature void(Cpu*) - the generated code's shape.
extern "C" JNIEXPORT jstring JNICALL
Java_io_github_powermoonrecomp_NativeGen_dlopenTest(JNIEnv *env, jobject, jstring soPath, jstring fnName) {
    std::string p = jstr(env, soPath), fn = jstr(env, fnName);
    void *h = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) return env->NewStringUTF((std::string("dlopen: ") + dlerror()).c_str());
    void (*f)(void *) = (void (*)(void *))dlsym(h, fn.c_str());
    if (!f) {
        dlclose(h);
        return env->NewStringUTF((std::string("dlsym: ") + dlerror()).c_str());
    }
    dlclose(h);
    return env->NewStringUTF("ok");
}
