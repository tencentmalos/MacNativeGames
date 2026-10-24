// SPDX-License-Identifier: GPL-2.0-or-later
package org.macnative.android

import androidx.test.platform.app.InstrumentationRegistry
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.File
import java.io.FilterInputStream
import java.io.InputStream

class ModuleRuntimeTest {
    @Test fun importedModulesExecuteAndFailedImportsPreserveSelection() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val title = context.getString(R.string.title_profile)
        val store = ModuleStore(context, title)
        val input = store.selected() ?: File(context.filesDir, "test-input")
        assumeTrue("Private prepared modules must be imported or supplied outside the APK", File(input, "manifest.json").isFile)
        val source = ModuleStore.Source { name -> File(input, name).inputStream() }
        val imported = JSONObject(store.import(source) {})
        assertEquals("pass", imported.getString("status"))
        assertEquals("validated", imported.getString("phase"))
        val selected = store.selected()!!
        val report = JSONObject(Native.modules(selected.absolutePath, title, true))
        android.util.Log.i("MacNativeTest", report.toString())
        assertEquals("pass", report.getString("status"))
        assertEquals(4000, report.getInt("calls"))
        assertEquals(4, report.getInt("sleep_calls"))
        assertEquals(4, report.getInt("threads"))
        val modules = report.getJSONArray("modules")
        for (i in 0 until modules.length()) assertTrue(modules.getJSONObject(i).getLong("base") < (1L shl 39))

        val other = if (title == "botw160") "totk100" else "botw160"
        assertEquals("fail", JSONObject(Native.modules(selected.absolutePath, other, false)).getString("status"))
        for (bad in listOf("manifest.json", "main.patched.bin", "sdk.patched.bin")) {
            try {
                store.import(ModuleStore.Source { name ->
                    if (name != bad) source.open(name)
                    else if (name == "sdk.patched.bin") ByteArrayInputStream(byteArrayOf(1))
                    else CorruptFirstByte(source.open(name))
                }) {}
                fail("Corrupt $bad was accepted")
            } catch (expected: IllegalArgumentException) {
                assertEquals(selected, store.selected())
            }
        }
        try {
            store.import(ModuleStore.Source { throw java.io.FileNotFoundException("Missing test input") }) {}
            fail("Missing files were accepted")
        } catch (expected: java.io.FileNotFoundException) { assertEquals(selected, store.selected()) }
        assertFalse(File(context.noBackupFilesDir, "modules").listFiles()!!.any { it.name.startsWith(".import-") })
        // A second complete native session must recreate and release its TLS keys
        // and mappings without depending on the previous session's addresses.
        assertEquals("pass", JSONObject(Native.modules(selected.absolutePath, title, true)).getString("status"))
    }

    private class CorruptFirstByte(input: InputStream) : FilterInputStream(input) {
        private var changed = false
        override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
            val n = super.read(buffer, offset, length)
            if (n > 0 && !changed) { buffer[offset] = (buffer[offset].toInt() xor 1).toByte(); changed = true }
            return n
        }
    }
}
