package io.github.powermoonrecomp

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import java.io.File

/**
 * Minimal pipeline proof: pick the game's exefs folder (SAF), generate the game code with
 * libnativegen, compile it with the bundled zig, and dlopen the result.
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
                append("generating...")
                val out = File(filesDir, "gen")
                val r = NativeGen.generateAll(exefs, out, "")
                append("generate: $r")
                if (r != "ok") return@Thread

                append("compiling with zig...")
                val zig = File(filesDir, "zig/zig")
                val so = File(filesDir, "odyssey_game.so")
                val cmd = arrayOf(
                    zig.absolutePath, "cc", "-target", "aarch64-linux-android",
                    "-O1", "-fno-strict-aliasing", "-Wno-return-type", "-Wno-unused-label",
                    "-fvisibility=hidden", "-shared", "-Wl,-undefined,dynamic_lookup",
                    "-Iruntime", "-o", so.absolutePath,
                    *out.listFiles()?.filter { it.extension == "c" }?.map { it.absolutePath }?.toTypedArray()
                        ?: emptyArray()
                )
                append(NativeGen.runCommand(cmd))

                append("dlopen test...")
                append(NativeGen.dlopenTest(so.absolutePath, "f_7000000000"))
            } catch (e: Exception) {
                append("error: ${e.message}")
            }
        }.start()
    }

    private fun append(s: String) {
        runOnUiThread { log.append(s + "\n") }
    }

    companion object {
        private const val REQ_TREE = 1
    }
}
