// SPDX-License-Identifier: GPL-2.0-or-later
package org.macnative.android

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.KeyEvent
import android.view.MotionEvent
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.ScrollView
import java.util.concurrent.Executors
import spatial.input.android.AndroidInputSource
import spatial.input.android.InputSink
import spatial.input.model.*

object Native {
    init { System.loadLibrary("macnative_platform") }
    external fun begin(): Long
    external fun end(token: Long)
    external fun device(token: Long, id: Long, epoch: Long, profile: String, buttons: IntArray, axes: IntArray, ranges: FloatArray): Boolean
    external fun packet(token: Long, id: Long, epoch: Long, sequence: Long, control: Int, kinds: IntArray, codes: IntArray, values: FloatArray): Boolean
    external fun status(): String
    external fun audioProbe(): String
    external fun vulkanProbe(directory: String): String
    external fun modules(directory: String, title: String, execute: Boolean): String
}

class PlatformActivity : Activity() {
    private val handler = Handler(Looper.getMainLooper())
    private val executor = Executors.newSingleThreadExecutor()
    private var collector: AndroidInputSource? = null
    private var token = 0L
    private lateinit var state: TextView
    private lateinit var work: RuntimeWork
    private lateinit var runtimeState: TextView
    private lateinit var importButton: Button
    private lateinit var verifyButton: Button
    private lateinit var shareButton: Button
    private val workChanged: () -> Unit = {
        runtimeState.text = work.status
        importButton.isEnabled = !work.busy
        verifyButton.isEnabled = !work.busy && work.store.selected() != null
        shareButton.isEnabled = work.report.isNotEmpty()
    }
    private val poll = object : Runnable {
        override fun run() { state.text = Native.status(); handler.postDelayed(this, 100) }
    }
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        work = RuntimeWork.get(this)
        val column = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(32,64,32,32) }
        column.addView(TextView(this).apply {
            text = "${applicationInfo.loadLabel(packageManager)}\n\nDevelopment preview — gameplay is not available yet.\nImport your prepared ${getString(R.string.game_version)} module folder, then verify the native runtime.\n"
        })
        runtimeState = TextView(this); column.addView(runtimeState)
        importButton = Button(this).apply {
            text = "Import prepared modules"
            setOnClickListener {
                startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION), 10)
            }
        }; column.addView(importButton)
        verifyButton = Button(this).apply { text = "Verify native runtime"; setOnClickListener { work.run() } }; column.addView(verifyButton)
        shareButton = Button(this).apply {
            text = "Share verification report"
            setOnClickListener { startActivity(Intent.createChooser(Intent(Intent.ACTION_SEND).setType("text/plain")
                .putExtra(Intent.EXTRA_TEXT, work.report), "Runtime verification report")) }
        }; column.addView(shareButton)
        state = TextView(this);column.addView(state)
        column.addView(Button(this).apply {
            text = "Play 250 ms audio test"
            setOnClickListener {
                isEnabled = false
                executor.execute {
                    val result = Native.audioProbe()
                    handler.post { if (!isDestroyed) { text = result; isEnabled = true } }
                }
            }
        })
        column.addView(Button(this).apply {
            text = "Verify Vulkan rendering"
            setOnClickListener {
                isEnabled = false
                executor.execute {
                    val directory = java.io.File(cacheDir, "nvn-self-test/shaders").apply { mkdirs() }
                    val result = Native.vulkanProbe(directory.absolutePath)
                    handler.post { if (!isDestroyed) { text = result; isEnabled = true } }
                }
            }
        })
        setContentView(ScrollView(this).apply { addView(column) })
        work.observe(workChanged)
    }
    @Deprecated("Activity result compatibility")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == 10 && resultCode == RESULT_OK) data?.data?.let { work.import(it) }
    }
    override fun onResume() {
        super.onResume()
        token = Native.begin()
        val session = token
        collector = AndroidInputSource(this, executor, object : InputSink {
            override fun onDeviceAvailable(device: DeviceIdentity, capabilities: DeviceCapabilities) {
                Native.device(session, device.backendId, device.connectionEpoch, device.profileKey,
                    capabilities.buttons.map { it.ordinal }.toIntArray(),
                    capabilities.axes.map { it.axis.ordinal }.toIntArray(),
                    capabilities.axes.flatMap { listOf(it.rawMin,it.rawMax,it.rawFlat) }.toFloatArray())
            }
            override fun onInputPacket(packet: InputPacket) {
                if (packet.protocolVersion != INPUT_PROTOCOL_VERSION) return
                val events = packet.events.filter { it.kind != InputEvent.Kind.SensorSample }
                Native.packet(session, packet.device.backendId, packet.device.connectionEpoch, packet.sequence,
                    packet.control.ordinal, events.map { it.kind.ordinal }.toIntArray(),
                    events.map { it.button?.ordinal ?: it.axis!!.ordinal }.toIntArray(),
                    events.map { if (it.kind == InputEvent.Kind.ButtonState) { if(it.pressed) 1f else 0f } else it.rawValue }.toFloatArray())
            }
        }).also { it.start(handler) }
        handler.post(poll)
    }
    override fun onPause() {
        handler.removeCallbacks(poll)
        collector?.stop();collector = null
        Native.end(token);token = 0
        super.onPause()
    }
    override fun onDestroy() { work.remove(workChanged);executor.shutdown();super.onDestroy() }
    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (!hasFocus) collector?.onFocusLost()
    }
    override fun dispatchKeyEvent(event: KeyEvent): Boolean = collector?.dispatchKeyEvent(event) == true || super.dispatchKeyEvent(event)
    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean = collector?.dispatchGenericMotionEvent(event) == true || super.dispatchGenericMotionEvent(event)
}
