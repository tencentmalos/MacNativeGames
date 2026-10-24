// SPDX-License-Identifier: MIT
package org.macnative.android

import android.content.Context
import android.annotation.SuppressLint
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.util.AtomicFile
import org.json.JSONObject
import java.io.File
import java.util.concurrent.Executors

/** Application-lifetime work survives an Activity recreation. Input has its own
 * executor, so importing or validating modules never queues controller packets.
 */
class RuntimeWork private constructor(context: Context) {
    private val context = context.applicationContext
    val store = ModuleStore(this.context, this.context.getString(R.string.title_profile))
    private val worker = Executors.newSingleThreadExecutor()
    private val main = Handler(Looper.getMainLooper())
    private val listeners = mutableSetOf<() -> Unit>()
    var busy = false; private set
    var status = if (store.selected() == null) "Import prepared modules to verify the native runtime."
                 else "Prepared modules are available. Ready for the native call check."; private set
    var report = ""; private set
    fun observe(listener: () -> Unit) { listeners.add(listener); listener() }
    fun remove(listener: () -> Unit) { listeners.remove(listener) }
    private fun changed() { listeners.toList().forEach { it() } }
    private fun submit(action: () -> String) {
        check(Looper.myLooper() == Looper.getMainLooper())
        if (busy) return
        busy = true; status = "Working…"; changed()
        worker.execute {
            val result = try { action() } catch (e: Exception) {
                JSONObject().put("status", "fail").put("error", e.message ?: e.javaClass.simpleName).toString()
            }
            val json = JSONObject(result)
            val saved = AtomicFile(File(context.filesDir, "last-runtime-report.json"))
            var persistenceError: String? = null
            var output: java.io.FileOutputStream? = null
            try { output = saved.startWrite(); output.write(result.toByteArray()); saved.finishWrite(output) }
            catch (e: Exception) { saved.failWrite(output); persistenceError = e.message }
            main.post {
                busy = false; report = result
                status = if (json.optString("status") != "pass") "Failed: ${json.optString("error")}" else
                    if (json.optString("phase") == "validated") "Modules imported and validated. Ready for the native call check."
                    else "Native runtime passed: ${json.optInt("calls")} timer calls, ${json.optInt("sleep_calls")} SDK waits, ${json.optInt("threads")} threads."
                if (persistenceError != null) status += "\nCould not save report: $persistenceError"
                changed()
            }
        }
    }
    fun import(uri: Uri) = submit {
        store.import(store.tree(uri)) { message -> main.post { status = message; changed() } }
    }
    fun run() = submit {
        val selected = store.selected() ?: error("Import the prepared modules first")
        Native.modules(selected.absolutePath, context.getString(R.string.title_profile), true)
    }
    companion object {
        // RuntimeWork retains applicationContext only; Activity listeners detach
        // in onDestroy, including when a job survives a configuration change.
        @SuppressLint("StaticFieldLeak")
        private var instance: RuntimeWork? = null
        @Synchronized fun get(context: Context): RuntimeWork = instance ?: RuntimeWork(context).also { instance = it }
    }
}
