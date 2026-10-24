// SPDX-License-Identifier: MIT
package org.macnative.android

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import android.util.AtomicFile
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.io.InputStream
import java.util.UUID

/** Copies only the four prepared module files into an app-private generation.
 * Failed or interrupted imports never replace the currently selected generation.
 * The original SAF tree and previous successful imports are left intact.
 */
class ModuleStore(private val context: Context, private val title: String) {
    fun interface Source { fun open(name: String): InputStream }
    private val root = File(context.noBackupFilesDir, "modules").apply { mkdirs() }
    private val current = AtomicFile(File(root, "current"))

    fun selected(): File? {
        val id = try { current.openRead().use { String(it.readBytes(), Charsets.UTF_8) } }
                 catch (_: java.io.IOException) { return null }
        if (!id.matches(Regex("[0-9a-f-]{36}"))) return null
        return File(root, id).takeIf { File(it, "manifest.json").isFile }
    }

    fun tree(uri: Uri): Source {
        require(DocumentsContract.isTreeUri(uri)) { "Select the prepared module folder" }
        val resolver = context.contentResolver
        val children = DocumentsContract.buildChildDocumentsUriUsingTree(uri, DocumentsContract.getTreeDocumentId(uri))
        val ids = mutableMapOf<String, String>()
        val names = setOf("manifest.json", "main.patched.bin", "subsdk0.patched.bin", "sdk.patched.bin")
        resolver.query(children, arrayOf(DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME), null, null, null)?.use { rows ->
            while (rows.moveToNext()) {
                val name = rows.getString(1)
                if (name in names) {
                    require(ids.put(name, rows.getString(0)) == null) { "Ambiguous file name: $name" }
                }
            }
        } ?: error("Cannot read the selected folder")
        require(ids.keys == names) { "Folder needs manifest.json and all three .patched.bin files" }
        return Source { name ->
            resolver.openInputStream(DocumentsContract.buildDocumentUriUsingTree(uri, ids.getValue(name)))
                ?: error("Cannot open $name")
        }
    }

    fun import(source: Source, progress: (String) -> Unit): String {
        val id = UUID.randomUUID().toString()
        val staging = File(root, ".import-$id")
        check(staging.mkdir()) { "Cannot create module import directory" }
        try {
            for (name in listOf("manifest.json", "main.patched.bin", "subsdk0.patched.bin", "sdk.patched.bin")) {
                val limit = if (name == "manifest.json") 64L shl 20 else 512L shl 20
                source.open(name).use { input ->
                    FileOutputStream(File(staging, name)).use { output ->
                        val buffer = ByteArray(128 * 1024)
                        var copied = 0L
                        var shown = -1L
                        while (true) {
                            if (Thread.currentThread().isInterrupted) throw InterruptedException("Import cancelled")
                            val n = input.read(buffer)
                            if (n < 0) break
                            copied += n
                            require(copied <= limit) { "$name exceeds the module size limit" }
                            output.write(buffer, 0, n)
                            val mib = copied shr 20
                            if (mib / 4 != shown) { shown = mib / 4; progress("Importing $name: $mib MiB") }
                        }
                        require(copied > 0) { "$name is empty" }
                        output.fd.sync()
                    }
                }
            }
            progress("Checking module hashes, relocations and memory permissions…")
            val report = Native.modules(staging.absolutePath, title, false)
            val result = JSONObject(report)
            require(result.optString("status") == "pass") { result.optString("error", "Module validation failed") }
            val ready = File(root, id)
            check(staging.renameTo(ready)) { "Cannot publish imported modules" }
            var output: FileOutputStream? = null
            try {
                output = current.startWrite()
                output.write(id.toByteArray(Charsets.UTF_8))
                current.finishWrite(output)
            } catch (e: Exception) {
                current.failWrite(output)
                throw e
            }
            return report
        } finally {
            // This is only our newly created, incomplete copy, never the source.
            if (staging.exists()) staging.deleteRecursively()
        }
    }
}
