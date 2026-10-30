// SPDX-License-Identifier: GPL-2.0-or-later
package org.macnative.android

import android.app.Activity
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.*
import android.widget.FrameLayout
import android.widget.TextView
import java.io.File
import java.util.concurrent.Executors
import spatial.input.android.AndroidInputSource
import spatial.input.android.InputSink
import spatial.input.model.*
import spatial.input.model.InputEvent

object GameNative {
    init { System.loadLibrary("macnative_platform") }
    external fun start(surface: Surface, modules: String, assets: String, shaders: String, profile: String, driver: String, hooks: String, driverDirectory: String, language: String, resolution: Int)
    external fun available(): Boolean
    external fun vsync(frameTimeNs: Long, refreshHz: Float)
    external fun key(code: Int, down: Boolean): Boolean
    external fun focus(focused: Boolean)
    external fun stop()
}

/** One native guest session per dedicated Android process. */
class GameActivity : Activity(), SurfaceHolder.Callback {
    private val handler = Handler(Looper.getMainLooper())
    private val inputWorker = Executors.newSingleThreadExecutor()
    private var collector: AndroidInputSource? = null
    private var token = 0L
    private var assets: GameAssets? = null
    private lateinit var settings: LaunchSettings
    private var driver = "turnip"
    private var language = "zh-Hans"
    private var resolution = 720
    private var driverDirectory = ""
    private var ready = false
    private var started = false
    private val vsyncCallback = object : Choreographer.FrameCallback {
        override fun doFrame(time: Long) { GameNative.vsync(time, display?.refreshRate ?: 60f); Choreographer.getInstance().postFrameCallback(this) }
    }
    private lateinit var overlay: PortOverlay
    private lateinit var surface: SurfaceView
    private lateinit var status: TextView
    private lateinit var modules: File
    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        if (BuildConfig.DEBUG) startService(android.content.Intent(this, PortDebugService::class.java))
        settings = LaunchSettings(this)
        Native.configureInput(intent.getBooleanExtra("swap-ab", settings.swapAB))
        driver = intent.getStringExtra("gpu-driver")?.takeIf { it in listOf("turnip", "system") } ?: settings.driver
        language = intent.getStringExtra("language")?.takeIf { value -> LaunchSettings.languages.any { it.first == value } } ?: settings.language
        resolution = intent.getIntExtra("render-resolution",settings.resolution).takeIf { it==720 || it==1080 } ?: 720
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        surface = SurfaceView(this).also { it.holder.setFixedSize(resolution*16/9,resolution);it.holder.addCallback(this) }
        overlay = PortOverlay(this, surface)
        status = TextView(this).apply { setTextColor(0xffffffff.toInt()); text = settings.text("正在准备 BOTW…", "Preparing BOTW…"); textSize = 18f; setPadding(32,32,32,32) }
        setContentView(FrameLayout(this).apply { addView(surface); addView(status) })
        window.decorView.post { window.insetsController?.hide(WindowInsets.Type.systemBars()) }
        if (!GameNative.available()) { status.text = settings.text("此构建未包含游戏运行环境", "Native game runtime is not included in this build"); return }
        modules = ModuleStore(this, "botw160").selected() ?: run { status.text = settings.text("请先导入 BOTW 模块", "Import BOTW modules first"); return }
        val source = getSharedPreferences("native-game", MODE_PRIVATE).getString("asset-tree", null)
            ?: run { status.text = settings.text("请先选择 BOTW 游戏文件夹", "Select the prepared BOTW game asset folder first"); return }
        Thread({
            try {
                if (driver == "turnip") driverDirectory = BundledDriver.prepare(this).path
                val view = GameAssets(this, Uri.parse(source)); assets = view
                view.prepare { progress -> handler.post { status.text = if(language.startsWith("zh")) "正在检查游戏资源…" else progress } }
                handler.post { ready = true; launch() }
            } catch (e: Exception) { handler.post { status.text = settings.text("无法启动 BOTW：", "Cannot start BOTW: ")+e.message } }
        }, "BOTW-assets").start()
    }
    private fun launch() {
        if (started || !ready || !surface.holder.surface.isValid || isFinishing) return
        started = true; status.visibility = View.GONE
        val profile = File(filesDir, "native-botw160").apply { mkdirs() }
        Thread({
            try { GameNative.start(surface.holder.surface, modules.path, assets!!.root.path, assets!!.shaderRoot.path, profile.path,
                driver, applicationInfo.nativeLibraryDir, driverDirectory, language, resolution) }
            catch(e:Exception) { handler.post { status.visibility=View.VISIBLE;status.text=settings.text("无法启动 BOTW：", "Cannot start BOTW: ")+e.message } }
        }, "BOTW-native").start()
    }
    override fun onConfigurationChanged(configuration: android.content.res.Configuration) {
        super.onConfigurationChanged(configuration)
        // Foundation's device listener handles input hotplug without replacing
        // the Surface or the process-lifetime native guest session.
        android.util.Log.i("BOTW-lifecycle", "Configuration updated in place: keyboard=${configuration.keyboard} navigation=${configuration.navigation}")
    }
    override fun surfaceCreated(holder: SurfaceHolder) = launch()
    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) = Unit
    override fun surfaceDestroyed(holder: SurfaceHolder) { if (started) GameNative.stop() }
    override fun onResume() {
        super.onResume(); overlay.resume(); token = Native.begin(); val session = token
        collector = AndroidInputSource(this, inputWorker, object : InputSink {
            override fun onDeviceAvailable(device: DeviceIdentity, capabilities: DeviceCapabilities) {
                Native.device(session, device.backendId, device.connectionEpoch, device.profileKey,
                    capabilities.buttons.map { it.ordinal }.toIntArray(), capabilities.axes.map { it.axis.ordinal }.toIntArray(),
                    capabilities.axes.flatMap { listOf(it.rawMin, it.rawMax, it.rawFlat) }.toFloatArray())
            }
            override fun onInputPacket(packet: InputPacket) {
                if (packet.protocolVersion != INPUT_PROTOCOL_VERSION) return
                val events = packet.events.filter { it.kind != InputEvent.Kind.SensorSample }
                Native.packet(session, packet.device.backendId, packet.device.connectionEpoch, packet.sequence,
                    packet.control.ordinal, events.map { it.kind.ordinal }.toIntArray(),
                    events.map { it.button?.ordinal ?: it.axis!!.ordinal }.toIntArray(),
                    events.map { if (it.kind == InputEvent.Kind.ButtonState) { if (it.pressed) 1f else 0f } else it.rawValue }.toFloatArray())
            }
        }).also { it.start(handler) }
        GameNative.focus(true)
        Choreographer.getInstance().postFrameCallback(vsyncCallback)
    }
    override fun onPause() { overlay.pause(); Choreographer.getInstance().removeFrameCallback(vsyncCallback); collector?.stop(); collector = null; Native.end(token); GameNative.focus(false); super.onPause() }
    override fun onWindowFocusChanged(focus: Boolean) { super.onWindowFocusChanged(focus); GameNative.focus(focus); if (!focus) {collector?.onFocusLost();OverlayNative.cancel()} }
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (overlay.key(event)) return true
        if (collector?.dispatchKeyEvent(event) == true) return true
        if (event.action == KeyEvent.ACTION_DOWN || event.action == KeyEvent.ACTION_UP)
            if (GameNative.key(event.keyCode, event.action == KeyEvent.ACTION_DOWN)) return true
        return super.dispatchKeyEvent(event)
    }
    override fun dispatchTouchEvent(event: MotionEvent): Boolean = overlay.touch(event)
    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean = collector?.dispatchGenericMotionEvent(event) == true || super.dispatchGenericMotionEvent(event)
    override fun onDestroy() { inputWorker.shutdown(); if (started) GameNative.stop(); assets?.close(); super.onDestroy(); GameNative.stop() }
}
