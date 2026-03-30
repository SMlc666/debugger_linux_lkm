package com.smlc666.lkmdbg

import android.os.Bundle
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.activity.ComponentActivity
import androidx.activity.enableEdgeToEdge
import androidx.core.view.setPadding
import androidx.lifecycle.lifecycleScope
import com.smlc666.lkmdbg.nativeui.NativeWorkspaceTextureView
import com.smlc666.lkmdbg.nativeui.toNativeWorkspaceSnapshot
import com.smlc666.lkmdbg.overlay.LkmdbgOverlayService
import com.smlc666.lkmdbg.overlay.OverlayPermission
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.launch

class MainActivity : ComponentActivity() {
    private lateinit var previewView: NativeWorkspaceTextureView
    private lateinit var statusView: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContentView(buildContentView())

        val repository = (application as LkmdbgApplication).sessionRepository
        lifecycleScope.launch {
            repository.state.collect { state ->
                previewView.updateSnapshot(state.toNativeWorkspaceSnapshot(expanded = true))
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
            }
        }
    }

    private fun buildContentView(): ScrollView {
        val density = resources.displayMetrics.density
        val padding = (20f * density).toInt()
        val previewHeight = (280f * density).toInt()

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
        val previewFrame = FrameLayout(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                previewHeight,
            )
        }
        previewView = NativeWorkspaceTextureView(this).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            )
        }
        statusView = TextView(this).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.START,
            )
            setPadding((12f * density).toInt())
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
            setOnClickListener { LkmdbgOverlayService.start(this@MainActivity) }
        }
        val hideButton = Button(this).apply {
            text = getString(R.string.overlay_action_hide)
            setOnClickListener { LkmdbgOverlayService.stop(this@MainActivity) }
        }

        previewFrame.addView(previewView)
        previewFrame.addView(statusView)
        buttonRow.addView(grantButton)
        buttonRow.addView(showButton)
        buttonRow.addView(hideButton)
        column.addView(titleView)
        column.addView(subtitleView)
        column.addView(previewFrame)
        column.addView(buttonRow)
        root.addView(column)
        return root
    }
}
