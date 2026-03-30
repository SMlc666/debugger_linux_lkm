package com.smlc666.lkmdbg

import android.content.ClipData
import android.content.ClipboardManager
import android.os.Bundle
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.activity.ComponentActivity
import androidx.core.view.setPadding
import androidx.core.view.WindowCompat
import androidx.lifecycle.lifecycleScope
import com.smlc666.lkmdbg.data.SessionBridgeRepository
import com.smlc666.lkmdbg.overlay.LkmdbgOverlayService
import com.smlc666.lkmdbg.overlay.OverlayPermission
import com.smlc666.lkmdbg.shell.BridgeStatusFormatter
import com.smlc666.lkmdbg.shell.SessionAutomationController
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.launch

class MainActivity : ComponentActivity() {
    private lateinit var statusView: TextView
    private lateinit var overlayView: TextView
    private lateinit var crashView: TextView
    private lateinit var repository: SessionBridgeRepository
    private lateinit var automation: SessionAutomationController

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        setContentView(buildContentView())
        refreshOverlayStatus()

        repository = (application as LkmdbgApplication).sessionRepository
        automation = SessionAutomationController(repository)
        automation.requestWarmStart(lifecycleScope)
        lifecycleScope.launch {
            repository.state.collect { state ->
                statusView.text = BridgeStatusFormatter.formatCompact(this@MainActivity, state)
                refreshOverlayStatus()
            }
        }
    }

    override fun onResume() {
        super.onResume()
        refreshOverlayStatus()
        refreshCrashStatus()
        automation.requestWarmStart(lifecycleScope)
    }

    private fun buildContentView(): ScrollView {
        val density = resources.displayMetrics.density
        val padding = (20f * density).toInt()

        val root = ScrollView(this)
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(padding)
        }
        val titleView = TextView(this).apply {
            text = getString(R.string.launcher_panel_title)
            textSize = 22f
        }
        val subtitleView = TextView(this).apply {
            text = getString(R.string.launcher_panel_body)
            textSize = 15f
            setPadding(0, (8f * density).toInt(), 0, (16f * density).toInt())
        }
        overlayView = TextView(this).apply {
            textSize = 14f
            setPadding(0, 0, 0, (12f * density).toInt())
            text = getString(
                R.string.overlay_permission_status,
                getString(if (OverlayPermission.hasPermission(this@MainActivity)) R.string.bool_yes else R.string.bool_no),
                getString(if (LkmdbgOverlayService.isRunning()) R.string.bool_yes else R.string.bool_no),
            )
        }
        statusView = TextView(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            )
            gravity = Gravity.START
            setPadding((12f * density).toInt(), (12f * density).toInt(), (12f * density).toInt(), (12f * density).toInt())
            textSize = 13f
        }
        crashView = TextView(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            )
            setPadding(0, (12f * density).toInt(), 0, 0)
            textSize = 12f
        }
        val buttonRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(0, (16f * density).toInt(), 0, 0)
        }
        val crashButtonRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(0, (12f * density).toInt(), 0, 0)
        }
        val grantButton = Button(this).apply {
            text = getString(R.string.overlay_action_grant)
            setOnClickListener { OverlayPermission.openSettings(this@MainActivity) }
        }
        val showButton = Button(this).apply {
            text = getString(R.string.overlay_action_show)
            setOnClickListener {
                LkmdbgOverlayService.start(this@MainActivity)
                refreshOverlayStatus()
            }
        }
        val hideButton = Button(this).apply {
            text = getString(R.string.overlay_action_hide)
            setOnClickListener {
                LkmdbgOverlayService.stop(this@MainActivity)
                refreshOverlayStatus()
            }
        }
        val copyCrashButton = Button(this).apply {
            text = getString(R.string.launcher_action_copy_last_crash)
            setOnClickListener { copyLastCrash() }
        }
        val clearCrashButton = Button(this).apply {
            text = getString(R.string.launcher_action_clear_last_crash)
            setOnClickListener {
                CrashLogger.clearLastCrash(this@MainActivity)
                refreshCrashStatus()
            }
        }

        buttonRow.addView(grantButton)
        buttonRow.addView(showButton)
        buttonRow.addView(hideButton)
        crashButtonRow.addView(copyCrashButton)
        crashButtonRow.addView(clearCrashButton)
        column.addView(titleView)
        column.addView(subtitleView)
        column.addView(overlayView)
        column.addView(statusView)
        column.addView(buttonRow)
        column.addView(crashView)
        column.addView(crashButtonRow)
        root.addView(column)
        refreshCrashStatus()
        return root
    }

    private fun refreshOverlayStatus() {
        if (!::overlayView.isInitialized)
            return
        overlayView.text = getString(
            R.string.overlay_permission_status,
            getString(if (OverlayPermission.hasPermission(this)) R.string.bool_yes else R.string.bool_no),
            getString(if (LkmdbgOverlayService.isRunning()) R.string.bool_yes else R.string.bool_no),
        )
    }

    private fun refreshCrashStatus() {
        if (!::crashView.isInitialized)
            return
        val report = CrashLogger.readLastCrash(this)
        if (report.isNullOrBlank()) {
            crashView.text = getString(R.string.launcher_last_crash_empty)
            return
        }
        val summary = report.lineSequence()
            .take(10)
            .joinToString("\n")
        crashView.text = getString(R.string.launcher_last_crash_summary, summary)
    }

    private fun copyLastCrash() {
        val report = CrashLogger.readLastCrash(this) ?: return
        val clipboard = getSystemService(ClipboardManager::class.java)
        clipboard?.setPrimaryClip(ClipData.newPlainText("lkmdbg-last-crash", report))
        refreshCrashStatus()
    }
}
