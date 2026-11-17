package io.github.powermoonrecomp

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import java.io.File

/**
 * Minimal pipeline proof: pick the game's exefs folder (SAF), generate the game code with
 * libnativegen, compile it with the bundled clang, and dlopen the result.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var log: TextView
    private var exefsDir: File? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        log = findViewById(R.id.log)
        findViewById<Button>(R.id.pick).setOnClickListener { pickExefs() }
        findViewById<Button>(R.id.run).setOnClickListener { runPipeline() }
    }

    private fun pickExefs() {
        val i = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
        startActivityForResult(i, REQ_TREE)
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == REQ_TREE && resultCode == Activity.RESULT_OK && data?.data != null) {
            val tree = data.data!!
            contentResolver.takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION)
            exefsDir = copyTreeToStorage(tree)
            append("exefs: ${exefsDir?.absolutePath}")
        }
    }

    /** Copy the picked tree's main/sdk/subsdk0 into app storage (SAF has no real paths). */
    private fun copyTreeToStorage(tree: Uri): File {
        val dest = File(filesDir, "exefs")
        dest.mkdirs()
        for ((name, _, _) in NativeGen.modules) {
            val doc = android.provider.DocumentsContract.buildDocumentUriUsingTree(
                tree, android.provider.DocumentsContract.getTreeDocumentId(tree) + "/" + name
            )
            try {
                contentResolver.openInputStream(doc)?.use { input ->
                    File(dest, name).outputStream().use { input.copyTo(it) }
                }
            } catch (e: Exception) {
                append("copy $name: ${e.message}")
            }
        }
        return dest
    }

    private fun runPipeline() {
        val exefs = exefsDir ?: run { append("pick the exefs folder first"); return }
        Thread {
            try {
                append("preparing compiler...")
                val compiler = extractCompiler() ?: run { append("compiler not bundled in this APK"); return@Thread }

                append("generating...")
                val out = File(filesDir, "gen")
                out.deleteRecursively()  // stale chunks from a previous run must not linger
                val r = NativeGen.generateAll(exefs, out, "")
                append("generate: $r")
                if (r != "ok") return@Thread

                append("compiling with clang...")
                val so = File(filesDir, "odyssey_game.so")
                val srcs = (out.listFiles()?.filter { it.extension == "c" }?.map { it.absolutePath } ?: emptyList()) +
                    File(compiler.runtime, "runtime_stub.c").absolutePath
                val cmd = listOf(
                    compiler.clang.absolutePath, "-target", "aarch64-linux-android24",
                    "--sysroot=${compiler.sysroot.absolutePath}",
                    "-O1", "-fno-strict-aliasing", "-Wno-return-type", "-Wno-unused-label",
                    "-shared", "-Wl,--allow-shlib-undefined",
                    "-I${compiler.runtime.absolutePath}", "-o", so.absolutePath,
                ) + srcs
                // The clang binary links against libc++_shared.so (bundled in compiler/lib);
                // the Android linker finds it via LD_LIBRARY_PATH.
                val env = mapOf(
                    "LD_LIBRARY_PATH" to compiler.lib.absolutePath,
                    "PATH" to "${compiler.bin.absolutePath}:${System.getenv("PATH") ?: ""}",
                )
                append(NativeGen.runProcess(cmd, env))

                // Android's linker refuses to dlopen a writable library (W^X policy).
                so.setWritable(false, false)
                append("dlopen test...")
                append(NativeGen.dlopenTest(so.absolutePath, "f_7000000000"))
            } catch (e: Exception) {
                append("error: ${e.message}")
            }
        }.start()
    }

    private class Compiler(
        val clang: File, val sysroot: File, val runtime: File, val bin: File, val lib: File
    )

    /** Copy the bundled compiler (assets are read-only) into filesDir and make it executable. */
    private fun extractCompiler(): Compiler? {
        val dir = File(filesDir, "compiler")
        val clang = File(dir, "bin/clang")
        if (clang.isFile() && clang.canExecute()) {
            return Compiler(clang, File(dir, "sysroot"), File(filesDir, "runtime"), File(dir, "bin"), File(dir, "lib"))
        }
        return try {
            // A previous run may have left a broken extraction (e.g. clang as a directory);
            // start clean so the asset copy below can write real files.
            dir.deleteRecursively()
            copyAssetTree("compiler", dir)
            // The asset tree may not preserve the clang-18 / ld.lld symlinks; recreate them.
            ensureLink(File(dir, "bin/clang"), File(dir, "bin/clang-18"))
            ensureLink(File(dir, "bin/lld"), File(dir, "bin/ld.lld"))
            for (f in listOf(clang, File(dir, "bin/clang-18"), File(dir, "bin/lld"), File(dir, "bin/ld.lld"))) {
                f.setExecutable(true, false)
                f.setReadable(true, false)
                f.setWritable(false, false)
            }
            copyAssetTree("runtime", File(filesDir, "runtime"))
            Compiler(clang, File(dir, "sysroot"), File(filesDir, "runtime"), File(dir, "bin"), File(dir, "lib"))
        } catch (e: Exception) {
            append("extract compiler: ${e.message}")
            null
        }
    }

    /** Make `link` exist as a symlink to `target` (fall back to a copy if symlinks fail). */
    private fun ensureLink(target: File, link: File) {
        if (link.exists()) return
        val ok = try {
            val p = ProcessBuilder("ln", "-s", target.absolutePath, link.absolutePath).start()
            p.waitFor() == 0 && link.exists()
        } catch (e: Exception) {
            false
        }
        if (!ok) target.copyTo(link)
    }

    /** Recursively copy an assets subdirectory to a filesDir location. */
    private fun copyAssetTree(assetPath: String, dest: File) {
        dest.mkdirs()
        val list = assets.list(assetPath) ?: return
        for (name in list) {
            val child = "$assetPath/$name"
            val out = File(dest, name)
            // assets.list() returns null for files, but on some Android versions it
            // returns an empty array for a file path - treat empty as a file too.
            val children = assets.list(child)
            if (children != null && children.isNotEmpty()) {
                copyAssetTree(child, out)
            } else {
                assets.open(child).use { input -> out.outputStream().use { input.copyTo(it) } }
            }
        }
    }

    private fun append(s: String) {
        android.util.Log.i(TAG, s)
        runOnUiThread { log.append(s + "\n") }
    }

    companion object {
        private const val REQ_TREE = 1
        private const val TAG = "PowerMoon"
    }
}
