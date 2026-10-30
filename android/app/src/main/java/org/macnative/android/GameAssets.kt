// SPDX-License-Identifier: MIT
package org.macnative.android

import android.content.Context
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract
import android.system.Os
import java.io.File
import java.security.MessageDigest
import org.json.JSONObject
import java.util.zip.ZipInputStream

/** Native runtime view of a user-authorized SAF tree. Large RomFS files remain
 * in their original location; only indexes, fonts and shader caches are copied.
 */
class GameAssets(private val context: Context, private val tree: Uri) : AutoCloseable {
    val root = File(context.noBackupFilesDir, "game-assets/botw160").apply { mkdirs() }
    lateinit var shaderRoot: File
        private set
    private val descriptors = mutableListOf<ParcelFileDescriptor>()
    private val directories = mutableMapOf<String, Map<String, String>>()
    private fun children(id: String): Map<String, String> = directories.getOrPut(id) {
        val result = mutableMapOf<String, String>()
        val uri = DocumentsContract.buildChildDocumentsUriUsingTree(tree, id)
        context.contentResolver.query(uri, arrayOf(DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME), null, null, null)?.use { rows ->
            while (rows.moveToNext()) check(result.put(rows.getString(1), rows.getString(0)) == null) { "Duplicate asset name" }
        } ?: error("Cannot read game asset folder")
        result
    }
    private fun id(path: String): String {
        var current = DocumentsContract.getTreeDocumentId(tree)
        for (part in path.split('/')) current = children(current)[part] ?: error("Missing asset: $path")
        return current
    }
    private fun uri(path: String) = DocumentsContract.buildDocumentUriUsingTree(tree, id(path))
    private fun hex(bytes: ByteArray): String {
        val digits = "0123456789abcdef"
        return CharArray(bytes.size * 2) { index ->
            val value = bytes[index / 2].toInt() and 255
            digits[if (index % 2 == 0) value ushr 4 else value and 15]
        }.concatToString()
    }
    private fun digest(file: File): String {
        val hash = MessageDigest.getInstance("SHA-256")
        file.inputStream().use { input ->
            val buffer = ByteArray(128 * 1024)
            while (true) { val n = input.read(buffer); if (n < 0) break; hash.update(buffer, 0, n) }
        }
        return hex(hash.digest())
    }
    private fun copy(path: String, limit: Long, file: File = File(root, path), expected: String? = null) {
        file.parentFile!!.mkdirs()
        if (expected != null && file.isFile && file.length() <= limit && digest(file) == expected) return
        val temporary = File(file.parentFile, file.name + ".import")
        try {
            context.contentResolver.openInputStream(uri(path))!!.use { input ->
                temporary.outputStream().use { output ->
                    val buffer = ByteArray(128 * 1024); var total = 0L
                    while (true) { val n = input.read(buffer); if (n < 0) break
                        total += n; check(total <= limit) { "Asset exceeds limit: $path" }; output.write(buffer, 0, n) }
                }
            }
            check(expected == null || digest(temporary) == expected) { "Asset hash mismatch: $path" }
            check(temporary.renameTo(file)) { "Cannot publish asset: $path" }
        } finally { temporary.delete() }
    }
    private fun image(path: String) {
        val fd = context.contentResolver.openFileDescriptor(uri(path), "r") ?: error("Cannot open $path")
        descriptors.add(fd)
        check(fd.statSize > 0) { "Game image must support seeking" }
        val link = File(root, path); link.parentFile!!.mkdirs()
        // This link is ours, never a copy of the shared image.
        if (link.exists() || runCatching { Os.lstat(link.path) }.isSuccess) Os.remove(link.path)
        Os.symlink("/proc/self/fd/${fd.fd}", link.path)
    }
    fun prepare(progress: (String) -> Unit) {
        for (folder in listOf("romfs", "aoc/1", "aoc/2")) {
            progress("Opening $folder")
            copy("$folder/runtime-index.bin", 64L shl 20); image("$folder/romfs.bin")
        }
        val manifestBytes = context.contentResolver.openInputStream(uri("shaders/manifest.json"))!!.use { input ->
            val data = ByteArray((4 shl 20) + 1); var count = 0
            while (count < data.size) {
                val n = input.read(data, count, data.size - count); if (n < 0) break; count += n
            }
            check(count <= (4 shl 20)) { "Shader manifest exceeds limit" }; data.copyOf(count)
        }
        val manifest = JSONObject(manifestBytes.toString(Charsets.UTF_8))
        check(manifest.getInt("schema") == 1 && manifest.getString("identity") == "sha256-control-code-prefix16") { "Unsupported shader bundle identity" }
        val files = manifest.getJSONObject("files")
        val shaderNames = files.keys().asSequence().toList().sorted()
        check(shaderNames.size in 1..50000 && shaderNames.all { it.matches(Regex("[0-9a-f]{16}\\.xg[sv]")) }) { "Invalid shader bundle inventory" }
        val generation = hex(MessageDigest.getInstance("SHA-256").digest(manifestBytes))
        shaderRoot = File(root, "shader-bundles/$generation/shaders")
        val missing = mutableSetOf<String>()
        for ((index, name) in shaderNames.withIndex()) {
            if (index % 100 == 0) progress("Checking shaders: $index/${shaderNames.size}")
            val expected = files.getString(name)
            check(expected.matches(Regex("[0-9a-f]{64}"))) { "Invalid shader digest: $name" }
            val cached = File(shaderRoot, name)
            if (!cached.isFile || cached.length() > (16L shl 20) || digest(cached) != expected) missing.add(name)
        }
        // One SAF stream avoids tens of thousands of provider/FUSE opens on
        // Android. Every extracted entry is still checked against the manifest.
        if (missing.isNotEmpty() && children(id("shaders")).containsKey("bundle.zip")) {
            shaderRoot.mkdirs()
            val seen = mutableSetOf<String>()
            ZipInputStream(context.contentResolver.openInputStream(uri("shaders/bundle.zip"))!!).use { zip ->
                val buffer = ByteArray(128 * 1024)
                while (true) {
                    val entry = zip.nextEntry ?: break
                    check(!entry.isDirectory && files.has(entry.name) && seen.add(entry.name)) { "Invalid shader archive entry" }
                    val hash = MessageDigest.getInstance("SHA-256")
                    val destination = File(shaderRoot, entry.name)
                    val temporary = File(shaderRoot, entry.name + ".import")
                    try {
                        temporary.outputStream().use { output ->
                            var size = 0L
                            while (true) {
                                val n = zip.read(buffer); if (n < 0) break
                                size += n; check(size <= (16L shl 20)) { "Shader exceeds limit" }
                                hash.update(buffer, 0, n); output.write(buffer, 0, n)
                            }
                        }
                        check(hex(hash.digest()) == files.getString(entry.name)) { "Shader archive hash mismatch: ${entry.name}" }
                        check(temporary.renameTo(destination)) { "Cannot publish shader" }
                    } finally { temporary.delete() }
                    missing.remove(entry.name)
                    if (seen.size % 100 == 0) progress("Importing shaders: ${seen.size}/${shaderNames.size}")
                }
            }
            check(missing.isEmpty()) { "Shader archive is incomplete" }
        } else {
            for (name in missing) copy("shaders/$name", 16L shl 20, File(shaderRoot, name), files.getString(name))
        }
        File(shaderRoot.parentFile!!, "manifest.json").writeBytes(manifestBytes)
        val fonts = children(id("shared-fonts")).keys.filter { it.endsWith(".bfttf") }.sorted()
        check(fonts.size == 6) { "Expected six verified system fonts" }
        for (name in fonts) copy("shared-fonts/$name", 32L shl 20)
    }
    override fun close() { descriptors.forEach { it.close() }; descriptors.clear() }
}
