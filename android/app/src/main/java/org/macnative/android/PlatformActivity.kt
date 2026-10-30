// SPDX-License-Identifier: GPL-2.0-or-later
package org.macnative.android

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.StateListDrawable
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.*
import android.widget.*
import java.io.File
import java.util.concurrent.Executors

object Native {
    init { System.loadLibrary("macnative_platform") }
    external fun configureInput(swapAB: Boolean)
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
    private val worker = Executors.newSingleThreadExecutor()
    private lateinit var settings: LaunchSettings
    private lateinit var work: RuntimeWork
    private lateinit var play: Button
    private lateinit var readiness: TextView
    private var diagnostic: TextView? = null
    private var optionsDialog: AlertDialog? = null
    private val cream = Color.rgb(239, 231, 207)
    private val muted = Color.rgb(182, 200, 190)
    private val gold = Color.rgb(213, 188, 127)
    private fun dp(value: Int) = (value * resources.displayMetrics.density).toInt()
    private fun tr(zh: String, en: String) = settings.text(zh, en)
    private fun label(value: String, size: Float, color: Int = cream) = TextView(this).apply {
        text = value; textSize = size; setTextColor(color); includeFontPadding = false
    }
    private fun shape(color: Int, border: Int = Color.TRANSPARENT) = GradientDrawable().apply {
        setColor(color); cornerRadius = dp(12).toFloat(); setStroke(dp(1), border)
    }
    private fun button(value: String, primary: Boolean = false, action: () -> Unit) = Button(this).apply {
        text = value; isAllCaps = false; isFocusable = true; textSize = 15f; typeface = Typeface.DEFAULT_BOLD
        setTextColor(if (primary) Color.rgb(20, 40, 32) else cream)
        minHeight = dp(48); minimumHeight = dp(48); minWidth = 0; minimumWidth = 0
        setPadding(dp(18), dp(8), dp(18), dp(8))
        background = StateListDrawable().apply {
            addState(intArrayOf(android.R.attr.state_focused), shape(if (primary) cream else 0xee31574a.toInt(), gold))
            addState(intArrayOf(android.R.attr.state_pressed), shape(if (primary) cream else 0xee31574a.toInt(), gold))
            addState(intArrayOf(), shape(if (primary) gold else 0xc91a302b.toInt(), if (primary) gold else 0x557d9c8b))
        }
        setOnClickListener { action() }
    }
    private val changed: () -> Unit = { updateReadiness(); diagnostic?.text = work.status }
    override fun onCreate(state: Bundle?) {
        super.onCreate(state); settings = LaunchSettings(this); work = RuntimeWork.get(this)
        window.statusBarColor = Color.TRANSPARENT; window.navigationBarColor = Color.BLACK
        render(); work.observe(changed)
    }
    private fun render() {
        if (getString(R.string.title_profile) != "botw160") {
            val page=LinearLayout(this).apply { orientation=LinearLayout.VERTICAL;setPadding(dp(32),dp(32),dp(32),dp(32)) }
            page.addView(label(getString(R.string.game_version),24f))
            page.addView(button(tr("导入已准备的模块", "Import prepared modules")) { chooseFolder(10) })
            page.addView(button(tr("诊断与验证", "Diagnostics and verification")) { showDiagnostics() })
            setContentView(page);return
        }
        val frame = FrameLayout(this).apply { setBackgroundColor(0xff10231e.toInt()) }
        val art = resources.getIdentifier("botw_background", "drawable", packageName)
        if (art != 0) frame.addView(ImageView(this).apply {
            setImageResource(art); scaleType = ImageView.ScaleType.CENTER_CROP; importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
        }, FrameLayout.LayoutParams(-1, -1))
        frame.addView(View(this).apply { background = GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
            intArrayOf(0xf20a1c18.toInt(), 0xa810231e.toInt(), 0x1810231e)) }, FrameLayout.LayoutParams(-1,-1))
        frame.addView(View(this).apply { background = GradientDrawable(GradientDrawable.Orientation.BOTTOM_TOP,
            intArrayOf(0xf90a1815.toInt(), 0x500a1815, Color.TRANSPARENT)) }, FrameLayout.LayoutParams(-1,-1))
        val page = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(32), dp(20), dp(32), dp(20)) }
        val top = LinearLayout(this).apply { gravity = Gravity.CENTER_VERTICAL }
        top.addView(label("NATIVE  /  ANDROID", 12f, muted).apply { letterSpacing = .18f }, LinearLayout.LayoutParams(0,-2,1f))
        top.addView(button(tr("设置", "Settings")) { showOptions() }, LinearLayout.LayoutParams(dp(92), dp(44)))
        page.addView(top)
        page.addView(View(this), LinearLayout.LayoutParams(1,0,1f))
        val identity = LinearLayout(this).apply { gravity = Gravity.CENTER_VERTICAL }
        val icon = resources.getIdentifier("botw_icon", "drawable", packageName)
        if (icon != 0) identity.addView(ImageView(this).apply {
            setImageResource(icon); scaleType = ImageView.ScaleType.CENTER_CROP
            background = shape(Color.TRANSPARENT); clipToOutline = true; contentDescription = null
        }, LinearLayout.LayoutParams(dp(64),dp(64)).apply { rightMargin = dp(16) })
        identity.addView(LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(label(tr("塞尔达传说", "THE LEGEND OF ZELDA"),14f,muted).apply { letterSpacing = .13f })
            addView(label(tr("旷野之息", "Breath of the Wild"),32f).apply { typeface = Typeface.create("sans-serif-light",Typeface.NORMAL) },
                LinearLayout.LayoutParams(-2,-2).apply { topMargin=dp(6) })
        })
        page.addView(identity)
        page.addView(label(tr("海拉鲁的旅程，继续。", "Your journey through Hyrule continues."),14f,muted),
            LinearLayout.LayoutParams(-2,-2).apply { topMargin=dp(16) })
        page.addView(View(this),LinearLayout.LayoutParams(1,0,1f))
        val launch = LinearLayout(this).apply { gravity=Gravity.CENTER_VERTICAL }
        play=button(tr("开始游戏  ›", "Start game  ›"),true) { launchGame() }
        play.contentDescription = tr("开始 BOTW 游戏", "Start BOTW")
        launch.addView(play,LinearLayout.LayoutParams(dp(220),dp(54)))
        readiness=label("",12f,muted)
        launch.addView(readiness,LinearLayout.LayoutParams(0,-2,1f).apply { leftMargin=dp(20) })
        launch.addView(label("BOTW 1.6.0",12f,muted))
        page.addView(launch)
        val controls = LinearLayout(this).apply { gravity = Gravity.CENTER_VERTICAL }
        val driverName=if(settings.driver=="turnip") "Turnip · 68dafb89" else tr("系统驱动", "System driver")
        controls.addView(button("GPU  ·  $driverName") { selectDriver() },LinearLayout.LayoutParams(0,dp(48),1f).apply { rightMargin=dp(10) })
        val language=LaunchSettings.languages.first { it.first==settings.language }.second
        controls.addView(button(tr("语言  ·  ","Language  ·  ")+language) { selectLanguage() },LinearLayout.LayoutParams(0,dp(48),1f).apply { rightMargin=dp(10) })
        controls.addView(label(tr("A 确认  ·  方向键选择", "A Select  ·  D-pad Navigate"),11f,muted))
        page.addView(controls,LinearLayout.LayoutParams(-1,-2).apply { topMargin=dp(14) })
        frame.addView(page,FrameLayout.LayoutParams(-1,-1));setContentView(frame)
        window.decorView.post { window.insetsController?.hide(WindowInsets.Type.systemBars()) }
        updateReadiness();play.post { if(play.isEnabled)play.requestFocus() }
    }
    private fun updateReadiness() {
        if (!::play.isInitialized) return
        val modules = work.store.selected()!=null
        val assets = getSharedPreferences("native-game",MODE_PRIVATE).getString("asset-tree",null)!=null
        readiness.text = when {
            work.busy -> tr("正在准备游戏文件…", "Preparing game files…")
            !modules -> tr("请在设置中导入游戏模块", "Import game modules in Settings")
            !assets -> tr("请在设置中选择游戏文件夹", "Select the game folder in Settings")
            else -> tr("●  已就绪", "●  Ready")
        }
        play.isEnabled = modules && assets && !work.busy && getString(R.string.title_profile)=="botw160"
        play.alpha=if(play.isEnabled)1f else .5f
        if(BuildConfig.DEBUG && play.isEnabled && intent.getBooleanExtra("test-launch",false)) {
            intent.removeExtra("test-launch")
            play.post { launchGame() }
        }
    }
    private fun launchGame() {
        startActivity(Intent(this,GameActivity::class.java).putExtra("gpu-driver",settings.driver).putExtra("language",settings.language).putExtra("swap-ab",settings.swapAB).putExtra("render-resolution",settings.resolution))
    }
    private fun selectDriver() {
        val values=arrayOf(tr("Turnip · shadPS4 同版（默认）", "Turnip · shadPS4 build (default)"),tr("系统 GPU 驱动", "System GPU driver"))
        AlertDialog.Builder(this).setTitle(tr("GPU 驱动", "GPU driver"))
            .setSingleChoiceItems(values,if(settings.driver=="turnip")0 else 1) { dialog,index ->
                settings.driver=if(index==0)"turnip" else "system";dialog.dismiss();render()
            }.setNegativeButton(tr("取消", "Cancel"),null).setMessage(null).show()
    }
    private fun selectResolution() {
        AlertDialog.Builder(this).setTitle(tr("运行模式 · 内部缩放 1×", "System mode · internal scale 1×"))
            .setSingleChoiceItems(arrayOf(tr("720p（手持模式）", "720p (Handheld)"), tr("1080p（底座模式）", "1080p (Docked)")),if(settings.resolution==720)0 else 1) { dialog,index ->
                settings.resolution=if(index==0)720 else 1080;dialog.dismiss();optionsDialog?.dismiss();render()
            }.setNegativeButton(tr("取消", "Cancel"),null).show()
    }
    private fun selectLanguage() {
        AlertDialog.Builder(this).setTitle(tr("游戏语言 · 下次启动生效", "Game language · next launch"))
            .setSingleChoiceItems(LaunchSettings.languages.map { it.second }.toTypedArray(),LaunchSettings.languages.indexOfFirst { it.first==settings.language }) { dialog,index ->
                settings.language=LaunchSettings.languages[index].first;dialog.dismiss();render()
            }.setNegativeButton(tr("取消", "Cancel"),null).show()
    }
    private fun chooseFolder(request: Int) {
        startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION), request)
    }
    private fun showOptions() {
        val column=LinearLayout(this).apply { orientation=LinearLayout.VERTICAL;setPadding(dp(24),dp(8),dp(24),dp(16)) }
        fun row(title:String, action:()->Unit) {column.addView(button(title,action=action),LinearLayout.LayoutParams(-1,dp(48)).apply { bottomMargin=dp(8) })}
        row(if(settings.resolution==720)tr("720p（手持模式）", "720p (Handheld)") else tr("1080p（底座模式）", "1080p (Docked)")) { selectResolution() }
        column.addView(label(tr("默认手持模式，内部缩放 1×；下次启动生效。", "Handheld by default, internal scale 1×; applies next launch."),12f,muted))
        column.addView(Switch(this).apply {
            text = tr("交换 A/B 键", "Swap A/B buttons"); isChecked = settings.swapAB
            setTextColor(cream); setPadding(0,dp(8),0,dp(8))
            setOnCheckedChangeListener { _, checked -> settings.swapAB = checked }
        },LinearLayout.LayoutParams(-1,dp(48)))
        column.addView(label(tr("默认按 NS 手柄的 A/B/X/Y 字母映射。此选项额外交换 A/B，默认关闭，下次启动生效。", "NS A/B/X/Y labels by default. This optionally swaps A/B; off by default, applies next launch."),12f,muted),
            LinearLayout.LayoutParams(-1,-2).apply { bottomMargin=dp(20) })
        column.addView(label(tr("游戏文件与工具", "Game files and tools"),15f),LinearLayout.LayoutParams(-1,-2).apply { bottomMargin=dp(16) })
        row(tr("导入已准备的 BOTW 模块", "Import prepared BOTW modules")) { if(!work.busy)chooseFolder(10) }
        row(tr("选择 BOTW 游戏文件夹", "Select BOTW game folder")) { if(!work.busy)chooseFolder(11) }
        row(tr("开源许可与素材来源", "Licenses and artwork")) { showNotices() }
        row(tr("诊断与验证", "Diagnostics and verification")) { optionsDialog?.dismiss();showDiagnostics() }
        column.addView(label(tr("驱动和语言在下一次启动游戏时生效。", "Driver and language apply on the next game launch."),12f,muted))
        optionsDialog=AlertDialog.Builder(this).setTitle(tr("设置", "Settings")).setView(ScrollView(this).apply { addView(column) })
            .setPositiveButton(tr("完成", "Done"),null).show()
    }
    private fun showNotices() {
        val names = assets.list("licenses")?.sorted().orEmpty()
        val source = tr("背景与图标：EmuPSP 缓存，Ghost / Nlib，BOTW 01007EF00011E000；版权归原权利人。", "Artwork: EmuPSP cache, Ghost / Nlib, BOTW 01007EF00011E000; copyright belongs to its respective owners.")
        val content = label(source+"\n\n"+names.joinToString("\n\n") { name -> name+"\n"+assets.open("licenses/$name").bufferedReader().use { it.readText() } },12f)
        content.setPadding(dp(24),dp(12),dp(24),dp(12));content.setTextIsSelectable(true)
        AlertDialog.Builder(this).setTitle(tr("许可与来源", "Licenses and sources"))
            .setView(ScrollView(this).apply { addView(content) }).setPositiveButton(tr("关闭", "Close"),null).show()
    }
    private fun showDiagnostics() {
        val column=LinearLayout(this).apply { orientation=LinearLayout.VERTICAL;setPadding(dp(24),dp(8),dp(24),dp(16)) }
        val output=label(work.status,12f);output.setTextIsSelectable(true);diagnostic=output
        column.addView(output)
        fun row(title:String, action:()->Unit) { column.addView(button(title,action=action),LinearLayout.LayoutParams(-1,-2).apply { topMargin=dp(8) }) }
        row(tr("验证运行环境", "Verify runtime")) { if(!work.busy)work.run() }
        row(tr("分享验证报告", "Share verification report")) { if(work.report.isNotEmpty())startActivity(Intent.createChooser(Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_TEXT,work.report),tr("验证报告", "Verification report"))) }
        row(tr("音频测试", "Audio test")) { worker.execute { val result=Native.audioProbe();handler.post { output.text=result } } }
        row(tr("系统 Vulkan 自检", "System Vulkan self-test")) { worker.execute {
            val result=Native.vulkanProbe(File(cacheDir,"nvn-self-test/shaders").apply { mkdirs() }.path);handler.post { output.text=result }
        } }
        row(tr("查看上次实际驱动", "Last active driver")) { output.text=runCatching { File(filesDir,"native-botw160/driver-status.txt").readText() }.getOrDefault(tr("尚无运行记录", "No game run recorded")) }
        AlertDialog.Builder(this).setTitle(tr("诊断", "Diagnostics")).setView(ScrollView(this).apply { addView(column) })
            .setPositiveButton(tr("关闭", "Close"),null).setOnDismissListener { diagnostic=null }.show()
    }
    @Deprecated("Activity result compatibility")
    override fun onActivityResult(requestCode:Int,resultCode:Int,data:Intent?) {
        super.onActivityResult(requestCode,resultCode,data)
        if(resultCode!=RESULT_OK)return
        data?.data?.let { uri ->
            if(requestCode==11) {
                try {
                    contentResolver.takePersistableUriPermission(uri,Intent.FLAG_GRANT_READ_URI_PERMISSION)
                    getSharedPreferences("native-game",MODE_PRIVATE).edit().putString("asset-tree",uri.toString()).apply()
                } catch(e:SecurityException) { Toast.makeText(this,tr("无法保留文件夹权限，请重新选择", "Cannot retain folder permission; select it again"),Toast.LENGTH_LONG).show() }
            }
            if(requestCode==10 && !work.busy)work.import(uri)
        }
        updateReadiness()
    }
    override fun onResume() {super.onResume();if(::work.isInitialized)updateReadiness()}
    override fun dispatchKeyEvent(event:KeyEvent):Boolean {
        if(event.keyCode==KeyEvent.KEYCODE_BUTTON_A && event.action==KeyEvent.ACTION_UP) return currentFocus?.performClick()==true
        return super.dispatchKeyEvent(event)
    }
    override fun onDestroy() {work.remove(changed);optionsDialog?.dismiss();worker.shutdown();super.onDestroy()}
}
