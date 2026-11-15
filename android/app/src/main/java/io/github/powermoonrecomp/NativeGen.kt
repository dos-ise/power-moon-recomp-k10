package io.github.powermoonrecomp

import android.content.Context
import java.io.File

/**
 * JNI wrapper over libnativegen (tools/nativegen/ngen.h) plus helpers to run the bundled clang
 * compiler and dlopen the compiled game code.
 */
object NativeGen {
    init {
        System.loadLibrary("nativegen_jni")
    }

    external fun generate(
        name: String, nsoPath: String, base: Long, outdir: String, hlePath: String, simd: String
    ): String

    external fun lastError(): String

    external fun dlopenTest(soPath: String, fnName: String): String

    /** Run a command (e.g. the bundled clang) and return its combined output. */
    fun runProcess(cmd: List<String>, env: Map<String, String> = emptyMap()): String {
        return try {
            val pb = ProcessBuilder(cmd)
            pb.environment().putAll(env)
            pb.redirectErrorStream(true)
            val p = pb.start()
            val out = p.inputStream.bufferedReader().readText()
            val rc = p.waitFor()
            out + "\n[exit $rc]"
        } catch (e: Exception) {
            "run failed: ${e.message}"
        }
    }

    /** The game's three NSO modules and their load addresses (as the PC build uses). */
    val modules = listOf(
        Triple("main", 0x7000000000L, "main"),
        Triple("subsdk0", 0x7003000000L, "subsdk0"),
        Triple("sdk", 0x7004000000L, "sdk"),
    )

    /** Copy the NSO files from a SAF-picked exefs folder into app storage. */
    fun copyExefs(context: Context, exefsDir: File, dest: File): String {
        dest.mkdirs()
        for ((name, _, _) in modules) {
            val src = File(exefsDir, name)
            if (!src.isFile) return "missing exefs/$name"
            src.copyTo(File(dest, name), overwrite = true)
        }
        return "ok"
    }

    /** Generate C for all modules into outdir. */
    fun generateAll(exefsDir: File, outdir: File, hlePath: String): String {
        outdir.mkdirs()
        for ((name, base, _) in modules) {
            val r = generate(name, File(exefsDir, name).absolutePath, base, outdir.absolutePath, hlePath, "asm")
            if (r != "ok") return "$name: $r"
        }
        return "ok"
    }
}
