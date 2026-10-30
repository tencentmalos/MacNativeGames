// SPDX-License-Identifier: MIT
package org.macnative.android

import android.app.Activity
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import android.os.Handler
import android.os.Looper
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View

object OverlayNative {
    init { System.loadLibrary("macnative_platform") }
    external fun touch(phase: Int, x: Float, y: Float)
    external fun key(code: Int, down: Boolean): Boolean
    external fun cancel()
    external fun battery(level: Int, current: Long, charge: Long, voltage: Int, temperature: Int, charging: Boolean)
}

/** Shared Android transport; all drawing, controls and preferences live in the native substrate. */
class PortOverlay(private val activity: Activity, private val surface: View) {
    private val handler = Handler(Looper.getMainLooper())
    private var primary = -1
    private val sample = object : Runnable {
        override fun run() {
            val manager = activity.getSystemService(BatteryManager::class.java)
            val intent = activity.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
            val level = intent?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
            val scale = intent?.getIntExtra(BatteryManager.EXTRA_SCALE, -1) ?: -1
            OverlayNative.battery(if (scale > 0 && level >= 0) level * 100 / scale else -1,
                manager.getLongProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW),
                manager.getLongProperty(BatteryManager.BATTERY_PROPERTY_CHARGE_COUNTER),
                intent?.getIntExtra(BatteryManager.EXTRA_VOLTAGE, -1) ?: -1,
                intent?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Int.MIN_VALUE) ?: Int.MIN_VALUE,
                manager.isCharging)
            handler.postDelayed(this, 1000)
        }
    }
    fun resume() { handler.removeCallbacks(sample); handler.post(sample) }
    fun pause() { handler.removeCallbacks(sample); primary = -1; OverlayNative.cancel() }
    fun key(event: KeyEvent): Boolean {
        if (event.action != KeyEvent.ACTION_DOWN && event.action != KeyEvent.ACTION_UP) return false
        // Repeated shortcut downs must not continuously open/close the panel.
        if (event.repeatCount > 0 && (event.keyCode == KeyEvent.KEYCODE_F10 || event.keyCode == KeyEvent.KEYCODE_BUTTON_MODE)) return true
        return OverlayNative.key(event.keyCode, event.action == KeyEvent.ACTION_DOWN)
    }
    fun touch(event: MotionEvent): Boolean {
        val index = event.actionIndex
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {primary = event.getPointerId(index); send(1, event, index)}
            MotionEvent.ACTION_MOVE -> {val at = event.findPointerIndex(primary); if (at >= 0) send(2, event, at)}
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> if (event.getPointerId(index) == primary) {send(3, event, index); primary = -1}
            MotionEvent.ACTION_CANCEL -> {OverlayNative.touch(4, 0f, 0f); primary = -1}
        }
        return true
    }
    private fun send(phase: Int, event: MotionEvent, index: Int) {
        val origin = IntArray(2); surface.getLocationOnScreen(origin)
        OverlayNative.touch(phase, (event.getRawX(index) - origin[0]) / surface.width.coerceAtLeast(1),
            (event.getRawY(index) - origin[1]) / surface.height.coerceAtLeast(1))
    }
}
