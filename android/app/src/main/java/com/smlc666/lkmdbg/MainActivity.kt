package com.smlc666.lkmdbg

import android.content.ClipData
import android.content.ClipboardManager
import android.os.Bundle
import android.widget.FrameLayout
import android.view.Gravity
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.activity.ComponentActivity
import androidx.core.view.setPadding
import androidx.core.view.WindowCompat
import androidx.lifecycle.lifecycleScope
import com.google.android.material.button.MaterialButton
import com.smlc666.lkmdbg.appdata.R
import com.smlc666.lkmdbg.data.SessionBridgeRepository
import com.smlc666.lkmdbg.overlay.LkmdbgOverlayService
import com.smlc666.lkmdbg.overlay.OverlayPermission
import com.smlc666.lkmdbg.shell.BridgeStatusFormatter
import com.smlc666.lkmdbg.shell.SessionAutomationController
import com.smlc666.lkmdbg.ui.theme.LkmdbgButtonTone
import com.smlc666.lkmdbg.ui.theme.loadLkmdbgColorRoles
import com.smlc666.lkmdbg.ui.theme.styleBody
import com.smlc666.lkmdbg.ui.theme.styleButton
import com.smlc666.lkmdbg.ui.theme.styleHeadline
import com.smlc666.lkmdbg.ui.theme.styleSurface
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.launch

class MainActivity : ComponentActivity() {
    private val roles by lazy { loadLkmdbgColorRoles(this) }
    private lateinit var statusView: TextView
    private lateinit var overlayView: TextView
    private lateinit var crashView: TextView
    private lateinit var diagnosticsView: TextView
    private lateinit var repository: SessionBridgeRepository
    private lateinit var automation: SessionAutomationController

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        setContentView(buildContentView())
        refreshOverlayStatus()

        repository = (application as LkmdbgApplication).sessionRepository
        automation = SessionAutomationController(repository)
        refreshBridgeDiagnostics()
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
        refreshBridgeDiagnostics()
        automation.requestWarmStart(lifecycleScope)
    }

    private fun buildContentView(): ScrollView {
        val density = resources.displayMetrics.density
        val padding = (20f * density).toInt()
        val root = ScrollView(this).apply {
            setBackgroundColor(roles.background)
        }
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(padding)
        }
        val titleView = TextView(this).apply {
            text = getString(R.string.launcher_panel_title)
            styleHeadline(this, roles, 24f)
        }
        val subtitleView = TextView(this).apply {
            text = getString(R.string.launcher_panel_body)
            styleBody(this, roles, 15f, secondary = true)
            setPadding(0, (8f * density).toInt(), 0, (16f * density).toInt())
        }
        overlayView = TextView(this).apply {
            styleBody(this, roles, 14f, secondary = true)
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
            styleBody(this, roles, 13f)
            styleSurface(this, roles, roles.surfaceContainer)
        }
        crashView = TextView(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            )
            setPadding(0, (12f * density).toInt(), 0, 0)
            styleBody(this, roles, 12f, secondary = true)
            styleSurface(this, roles, roles.surfaceContainerHigh)
        }
        diagnosticsView = TextView(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            )
            setPadding(0, (12f * density).toInt(), 0, 0)
            styleBody(this, roles, 12f, secondary = true)
            styleSurface(this, roles, roles.surfaceContainerHigh)
        }
        val buttonRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(0, (16f * density).toInt(), 0, 0)
        }
        val crashButtonRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(0, (12f * density).toInt(), 0, 0)
        }
        val grantButton = MaterialButton(this).apply {
            text = getString(R.string.overlay_action_grant)
            styleButton(this, roles, LkmdbgButtonTone.Outlined)
            setOnClickListener { OverlayPermission.openSettings(this@MainActivity) }
        }
        val showButton = MaterialButton(this).apply {
            text = getString(R.string.overlay_action_show)
            styleButton(this, roles, LkmdbgButtonTone.Filled)
            setOnClickListener {
                LkmdbgOverlayService.start(this@MainActivity)
                refreshOverlayStatus()
            }
        }
        val hideButton = MaterialButton(this).apply {
            text = getString(R.string.overlay_action_hide)
            styleButton(this, roles, LkmdbgButtonTone.Tonal)
            setOnClickListener {
                LkmdbgOverlayService.stop(this@MainActivity)
                refreshOverlayStatus()
            }
        }
        val copyCrashButton = MaterialButton(this).apply {
            text = getString(R.string.launcher_action_copy_last_crash)
            styleButton(this, roles, LkmdbgButtonTone.Outlined)
            setOnClickListener { copyLastCrash() }
        }
        val clearCrashButton = MaterialButton(this).apply {
            text = getString(R.string.launcher_action_clear_last_crash)
            styleButton(this, roles, LkmdbgButtonTone.Tonal)
            setOnClickListener {
                CrashLogger.clearLastCrash(this@MainActivity)
                refreshCrashStatus()
            }
        }

        buttonRow.addView(materialRowCell(grantButton))
        buttonRow.addView(materialRowCell(showButton))
        buttonRow.addView(materialRowCell(hideButton))
        crashButtonRow.addView(materialRowCell(copyCrashButton))
        crashButtonRow.addView(materialRowCell(clearCrashButton))
        column.addView(titleView)
        column.addView(subtitleView)
        column.addView(overlayView)
        column.addView(statusView)
        column.addView(buttonRow)
        column.addView(diagnosticsView)
        column.addView(crashView)
        column.addView(crashButtonRow)
        root.addView(column)
        refreshBridgeDiagnostics()
        refreshCrashStatus()
        return root
    }

    private fun materialRowCell(button: MaterialButton): FrameLayout =
        FrameLayout(this).apply {
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                marginEnd = (6f * resources.displayMetrics.density).toInt()
            }
            addView(
                button,
                FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                ),
            )
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

    private fun refreshBridgeDiagnostics() {
        if (!::diagnosticsView.isInitialized || !::repository.isInitialized)
            return
        val diagnostics = repository.rootBridgeDiagnostics()
        val candidates = diagnostics.suCandidates.take(5).joinToString(" | ")
        diagnosticsView.text = getString(
            R.string.launcher_root_diagnostics,
            diagnostics.uid,
            diagnostics.agentPath,
            candidates,
        )
    }

    private fun copyLastCrash() {
        val report = CrashLogger.readLastCrash(this) ?: return
        val clipboard = getSystemService(ClipboardManager::class.java)
        clipboard?.setPrimaryClip(ClipData.newPlainText("lkmdbg-last-crash", report))
        refreshCrashStatus()
    }
}
