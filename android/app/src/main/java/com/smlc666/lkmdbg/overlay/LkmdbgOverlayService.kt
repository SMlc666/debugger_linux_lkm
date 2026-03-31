package com.smlc666.lkmdbg.overlay

import android.content.Context
import android.content.Intent
import android.os.IBinder
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import androidx.lifecycle.LifecycleService
import androidx.lifecycle.lifecycleScope
import com.smlc666.lkmdbg.CrashLogger
import com.smlc666.lkmdbg.LkmdbgApplication
import com.smlc666.lkmdbg.data.SessionBridgeRepository
import com.smlc666.lkmdbg.nativeui.NativeWorkspaceTextureView
import com.smlc666.lkmdbg.nativeui.toNativeWorkspaceSnapshot
import com.smlc666.lkmdbg.shell.AppIconLoader
import com.smlc666.lkmdbg.shell.BridgeStatusFormatter
import com.smlc666.lkmdbg.shell.SessionAutomationController
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.roundToInt

class LkmdbgOverlayService : LifecycleService() {
    private lateinit var windowManager: WindowManager
    private lateinit var windowController: OverlayWindowController
    private lateinit var repository: SessionBridgeRepository
    private lateinit var automation: SessionAutomationController
    private lateinit var processPickerController: OverlayProcessPickerController
    private var rootView: FrameLayout? = null
    private var workspaceView: NativeWorkspaceTextureView? = null
    private var statusView: TextView? = null
    private var overlayJob: Job? = null
    private var layoutParams: WindowManager.LayoutParams? = null
    private var expanded = false

    override fun onBind(intent: Intent): IBinder? = super.onBind(intent)

    override fun onCreate() {
        super.onCreate()
        running.set(true)
        try {
            repository = (application as LkmdbgApplication).sessionRepository
            automation = SessionAutomationController(repository)
            windowManager = getSystemService(WINDOW_SERVICE) as WindowManager
            windowController = OverlayWindowController(resources, windowManager)
            processPickerController = OverlayProcessPickerController(
                context = this,
                repository = repository,
                iconLoader = AppIconLoader(this),
            ) { action ->
                lifecycleScope.launch {
                    action()
                }
            }
            automation.requestWarmStart(lifecycleScope)
            automation.startStatusLoop(lifecycleScope)
            createOverlay()
        } catch (t: Throwable) {
            CrashLogger.logHandled(this, "overlay_on_create", t)
            running.set(false)
            stopSelf()
            throw t
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopSelf()
            return START_NOT_STICKY
        }
        return START_STICKY
    }

    override fun onDestroy() {
        overlayJob?.cancel()
        if (::automation.isInitialized)
            automation.stopStatusLoop()
        removeOverlay()
        running.set(false)
        super.onDestroy()
    }

    private fun createOverlay() {
        if (rootView != null)
            return
        windowController.initialize()
        mountOverlay(expanded)
    }

    private fun mountOverlay(expanded: Boolean) {
        val params = windowController.createLayoutParams(expanded)
        val density = resources.displayMetrics.density
        val root = FrameLayout(this).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            )
        }
        val workspaceTopInset = if (expanded) windowController.expandedWorkspaceTopInsetPx() else 0
        val nativeView = NativeWorkspaceTextureView(this).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            ).apply {
                topMargin = workspaceTopInset
            }
            setOnTouchListener { _: View, event: MotionEvent ->
                if (expanded)
                    return@setOnTouchListener dispatchNativeTouch(event)
                when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN -> {
                        lastTouchRawX = event.rawX
                        lastTouchRawY = event.rawY
                        dragDistancePx = 0f
                        true
                    }

                    MotionEvent.ACTION_MOVE -> {
                        val dx = event.rawX - lastTouchRawX
                        val dy = event.rawY - lastTouchRawY
                        lastTouchRawX = event.rawX
                        lastTouchRawY = event.rawY
                        dragDistancePx += kotlin.math.abs(dx) + kotlin.math.abs(dy)
                        val view = rootView
                        val currentParams = layoutParams
                        if (view != null && currentParams != null)
                            windowController.moveCollapsed(view, currentParams, dx, dy)
                        true
                    }

                    MotionEvent.ACTION_UP -> {
                        if (dragDistancePx <= 12f * density)
                            updateExpandedState(true)
                        true
                    }

                    MotionEvent.ACTION_CANCEL -> {
                        dragDistancePx = 0f
                        true
                    }

                    else -> false
                }
            }
        }
        root.addView(nativeView)

        var overlayStatusView: TextView? = null
        if (expanded) {
            val header = LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                layoutParams = FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                    Gravity.TOP,
                )
                setPadding((14f * density).toInt(), (14f * density).toInt(), (14f * density).toInt(), (14f * density).toInt())
            }
            val statusRow = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                )
            }
            overlayStatusView = TextView(this).apply {
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                textSize = 13f
            }
            val collapseButton = Button(this).apply {
                text = getString(com.smlc666.lkmdbg.R.string.overlay_action_collapse)
                setOnClickListener { updateExpandedState(false) }
            }
            val closeButton = Button(this).apply {
                text = getString(com.smlc666.lkmdbg.R.string.overlay_action_close)
                setOnClickListener { stopSelf() }
            }
            statusRow.addView(overlayStatusView)
            statusRow.addView(collapseButton)
            statusRow.addView(closeButton)
            val actionRowTop = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                )
                setPadding(0, (8f * density).toInt(), 0, 0)
            }
            val actionRowBottom = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                )
                setPadding(0, (6f * density).toInt(), 0, 0)
            }
            actionRowTop.addView(makeActionButton(com.smlc666.lkmdbg.R.string.session_action_connect) {
                repository.connect()
            })
            actionRowTop.addView(makeActionButton(com.smlc666.lkmdbg.R.string.session_action_open_session) {
                repository.openSession()
            })
            actionRowTop.addView(makeActionButton(com.smlc666.lkmdbg.R.string.session_action_refresh) {
                repository.refreshStatus()
            })
            actionRowBottom.addView(makeActionButton(com.smlc666.lkmdbg.R.string.process_action_refresh) {
                repository.refreshProcesses()
            })
            actionRowBottom.addView(makeActionButton(com.smlc666.lkmdbg.R.string.process_action_attach) {
                processPickerController.toggle(repository.state.value)
            })
            actionRowBottom.addView(makeActionButton(com.smlc666.lkmdbg.R.string.event_action_refresh) {
                repository.refreshEvents(timeoutMs = 0, maxEvents = 16)
            })
            header.addView(statusRow)
            header.addView(actionRowTop)
            header.addView(actionRowBottom)
            root.addView(header)
            root.addView(processPickerController.build(density))
        }

        workspaceView = nativeView
        statusView = overlayStatusView
        rootView = root
        layoutParams = params
        windowManager.addView(root, params)
        overlayJob?.cancel()
        overlayJob = lifecycleScope.launch {
            repository.state.collect { state ->
                workspaceView?.updateSnapshot(
                    state.toNativeWorkspaceSnapshot(this@LkmdbgOverlayService, expanded),
                )
                statusView?.text = BridgeStatusFormatter.formatOverlayStatus(this@LkmdbgOverlayService, state)
                processPickerController.render(state)
            }
        }
    }

    private fun makeActionButton(textRes: Int, action: suspend () -> Unit): Button =
        Button(this).apply {
            text = getString(textRes)
            textSize = 12f
            layoutParams = LinearLayout.LayoutParams(
                0,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                1f,
            ).apply {
                marginEnd = (6f * resources.displayMetrics.density).roundToInt()
            }
            setOnClickListener {
                lifecycleScope.launch {
                    action()
                }
            }
        }

    private var lastTouchRawX = 0f
    private var lastTouchRawY = 0f
    private var dragDistancePx = 0f

    private fun updateExpandedState(nextExpanded: Boolean) {
        if (expanded == nextExpanded)
            return
        expanded = nextExpanded
        overlayJob?.cancel()
        rootView?.let { view ->
            runCatching { windowManager.removeViewImmediate(view) }
        }
        rootView = null
        workspaceView = null
        statusView = null
        layoutParams = null
        mountOverlay(expanded)
    }

    private fun removeOverlay() {
        overlayJob?.cancel()
        rootView?.let { view ->
            runCatching { windowManager.removeView(view) }
        }
        rootView = null
        workspaceView = null
        statusView = null
        layoutParams = null
        if (::processPickerController.isInitialized)
            processPickerController.clear()
    }

    companion object {
        private const val ACTION_SHOW = "com.smlc666.lkmdbg.overlay.SHOW"
        private const val ACTION_STOP = "com.smlc666.lkmdbg.overlay.STOP"
        private val running = AtomicBoolean(false)

        fun isRunning(): Boolean = running.get()

        fun start(context: Context) {
            if (!OverlayPermission.hasPermission(context))
                return
            val intent = Intent(context, LkmdbgOverlayService::class.java).setAction(ACTION_SHOW)
            runCatching {
                context.startService(intent)
            }.onFailure { throwable ->
                CrashLogger.logHandled(context, "overlay_start", throwable)
            }
        }

        fun stop(context: Context) {
            val intent = Intent(context, LkmdbgOverlayService::class.java).setAction(ACTION_STOP)
            runCatching {
                context.startService(intent)
            }.onFailure { throwable ->
                CrashLogger.logHandled(context, "overlay_stop", throwable)
            }
        }
    }

}
