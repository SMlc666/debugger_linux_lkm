package com.smlc666.lkmdbg

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
import com.smlc666.lkmdbg.overlay.LkmdbgOverlayService
import com.smlc666.lkmdbg.overlay.OverlayPermission
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.launch

class MainActivity : ComponentActivity() {
    private lateinit var statusView: TextView
    private lateinit var overlayView: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        setContentView(buildContentView())
        refreshOverlayStatus()

        val repository = (application as LkmdbgApplication).sessionRepository
        lifecycleScope.launch {
            repository.state.collect { state ->
                statusView.text = buildString {
                    append("transport=")
                    append(state.snapshot.transport)
                    append('\n')
                    append("pid=")
                    append(state.snapshot.targetPid)
                    append(" tid=")
                    append(state.snapshot.targetTid)
                    append(" sid=0x")
                    append(state.snapshot.sessionId.toString(16))
                    append('\n')
                    append("proc=")
                    append(state.processes.size)
                    append(" thr=")
                    append(state.threads.size)
                    append(" evt=")
                    append(state.recentEvents.size)
                    append(" hook=")
                    append(state.snapshot.hookActive)
                    append('\n')
                    append(state.lastMessage)
                }
                refreshOverlayStatus()
            }
        }
    }

    override fun onResume() {
        super.onResume()
        refreshOverlayStatus()
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
        val buttonRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(0, (16f * density).toInt(), 0, 0)
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

        buttonRow.addView(grantButton)
        buttonRow.addView(showButton)
        buttonRow.addView(hideButton)
        column.addView(titleView)
        column.addView(subtitleView)
        column.addView(overlayView)
        column.addView(statusView)
        column.addView(buttonRow)
        root.addView(column)
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
}
