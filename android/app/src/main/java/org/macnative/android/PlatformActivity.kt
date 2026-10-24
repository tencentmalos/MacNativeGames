// SPDX-License-Identifier: GPL-2.0-or-later
package org.macnative.android

import android.app.Activity
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.KeyEvent
import android.view.MotionEvent
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
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
}

class PlatformActivity : Activity() {
    private val handler = Handler(Looper.getMainLooper())
    private val executor = Executors.newSingleThreadExecutor()
    private var collector: AndroidInputSource? = null
    private var token = 0L
    private lateinit var state: TextView
    private val poll = object : Runnable {
        override fun run() { state.text = Native.status(); handler.postDelayed(this, 100) }
    }
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val column = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(32,64,32,32) }
        column.addView(TextView(this).apply {
            text = "${applicationInfo.loadLabel(packageManager)}\n\nFoundation controller and audio validation.\nGame startup and Vulkan rendering are not connected yet.\n"
        })
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
        setContentView(column)
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
    override fun onDestroy() { executor.shutdown();super.onDestroy() }
    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (!hasFocus) collector?.onFocusLost()
    }
    override fun dispatchKeyEvent(event: KeyEvent): Boolean = collector?.dispatchKeyEvent(event) == true || super.dispatchKeyEvent(event)
    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean = collector?.dispatchGenericMotionEvent(event) == true || super.dispatchGenericMotionEvent(event)
}
