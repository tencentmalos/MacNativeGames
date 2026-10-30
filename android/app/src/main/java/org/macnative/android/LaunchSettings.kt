// SPDX-License-Identifier: MIT
package org.macnative.android

import android.content.Context
import org.json.JSONObject
import java.io.File
import java.security.MessageDigest

/** Stored in the launcher; immutable values are passed to each new game process. */
class LaunchSettings(context: Context) {
    private val preferences = context.getSharedPreferences("native-game", Context.MODE_PRIVATE)
    var driver: String
        get() = preferences.getString("gpu-driver", "turnip").takeIf { it in listOf("turnip", "system") } ?: "turnip"
        set(value) { require(value in listOf("turnip", "system")); preferences.edit().putString("gpu-driver", value).apply() }
    var language: String
        get() = preferences.getString("language", "zh-Hans").takeIf { it in languages.map { p -> p.first } } ?: "zh-Hans"
        set(value) { require(value in languages.map { it.first }); preferences.edit().putString("language", value).apply() }
    var resolution: Int
        get() = preferences.getInt("render-resolution", 720).takeIf { it == 720 || it == 1080 } ?: 720
        set(value) { require(value == 720 || value == 1080); preferences.edit().putInt("render-resolution", value).apply() }
    var swapAB: Boolean
        get() = preferences.getBoolean("swap-ab", false)
        set(value) { preferences.edit().putBoolean("swap-ab", value).apply() }
    val chinese get() = language.startsWith("zh")
    fun text(zh: String, en: String) = if (chinese) zh else en
    companion object {
        val languages = listOf("zh-Hans" to "简体中文", "zh-Hant" to "繁體中文", "en-US" to "English", "ja" to "日本語")
    }
}

object BundledDriver {
    fun identity(context: Context): JSONObject = JSONObject(context.assets.open("turnip/identity.json").bufferedReader().use { it.readText() })
    // Content-addressed immutable extraction; verify both the asset and an
    // existing copy. A corrupt driver never silently falls back to another ICD.
    fun prepare(context: Context): File {
        val expected = identity(context).getString("library_sha256")
        require(expected.matches(Regex("[0-9a-f]{64}")))
        val root = File(context.filesDir, "gpu-drivers/$expected").apply { mkdirs() }
        val target = File(root, "vulkan.ad07xx.so")
        fun sha(file: File): String {
            val hash = MessageDigest.getInstance("SHA-256")
            file.inputStream().use { stream -> val block = ByteArray(65536); while (true) { val n = stream.read(block); if (n < 0) break; hash.update(block, 0, n) } }
            return hash.digest().joinToString("") { "%02x".format(it) }
        }
        if (target.isFile && sha(target) == expected) return root
        val staging = File(root, "driver.part")
        try {
            context.assets.open("turnip/vulkan.ad07xx.so").use { input -> staging.outputStream().use { input.copyTo(it) } }
            check(sha(staging) == expected) { "Bundled Turnip SHA-256 mismatch" }
            check(staging.renameTo(target)) { "Cannot publish verified driver" }
        } finally { staging.delete() }
        return root
    }
}
