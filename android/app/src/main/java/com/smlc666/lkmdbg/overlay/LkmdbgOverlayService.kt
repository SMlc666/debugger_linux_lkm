package com.smlc666.lkmdbg.overlay

import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.graphics.PixelFormat
import android.os.IBinder
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.Button
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.lifecycle.LifecycleService
import androidx.lifecycle.lifecycleScope
import com.smlc666.lkmdbg.CrashLogger
import com.smlc666.lkmdbg.LkmdbgApplication
import com.smlc666.lkmdbg.data.ProcessFilter
import com.smlc666.lkmdbg.data.ResolvedProcessRecord
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
    private lateinit var repository: SessionBridgeRepository
    private lateinit var automation: SessionAutomationController
    private lateinit var iconLoader: AppIconLoader
    private var rootView: FrameLayout? = null
    private var workspaceView: NativeWorkspaceTextureView? = null
    private var statusView: TextView? = null
    private var processPickerView: FrameLayout? = null
    private var processPickerSummaryView: TextView? = null
    private var processPickerListView: LinearLayout? = null
    private var overlayJob: Job? = null
    private var layoutParams: WindowManager.LayoutParams? = null
    private var collapsedDiameterPx = 0
    private var collapsedX = 0
    private var collapsedY = 0
    private var expanded = false

    override fun onBind(intent: Intent): IBinder? = super.onBind(intent)

    override fun onCreate() {
        super.onCreate()
        running.set(true)
        try {
            repository = (application as LkmdbgApplication).sessionRepository
            automation = SessionAutomationController(repository)
            iconLoader = AppIconLoader(this)
            windowManager = getSystemService(WINDOW_SERVICE) as WindowManager
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

        val density = resources.displayMetrics.density
        collapsedDiameterPx = (72f * density).roundToInt()
        collapsedX = (18f * density).roundToInt()
        collapsedY = (96f * density).roundToInt()
        mountOverlay(expanded)
    }

    private fun mountOverlay(expanded: Boolean) {
        val params = createLayoutParams(expanded)
        val density = resources.displayMetrics.density
        val root = FrameLayout(this).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            )
        }
        val nativeView = NativeWorkspaceTextureView(this).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            )
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
                        moveCollapsedBall(dx, dy)
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
        var overlayProcessPicker: FrameLayout? = null
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
                toggleProcessPicker()
            })
            actionRowBottom.addView(makeActionButton(com.smlc666.lkmdbg.R.string.event_action_refresh) {
                repository.refreshEvents(timeoutMs = 0, maxEvents = 16)
            })
            header.addView(statusRow)
            header.addView(actionRowTop)
            header.addView(actionRowBottom)
            root.addView(header)
            overlayProcessPicker = buildProcessPicker(density)
            root.addView(overlayProcessPicker)
        }

        workspaceView = nativeView
        statusView = overlayStatusView
        processPickerView = overlayProcessPicker
        rootView = root
        layoutParams = params
        windowManager.addView(root, params)
        overlayJob?.cancel()
        overlayJob = lifecycleScope.launch {
            repository.state.collect { state ->
                workspaceView?.updateSnapshot(state.toNativeWorkspaceSnapshot(expanded))
                statusView?.text = BridgeStatusFormatter.formatOverlayStatus(this@LkmdbgOverlayService, state)
                renderProcessPicker(state)
            }
        }
    }

    private fun buildProcessPicker(density: Float): FrameLayout {
        val container = FrameLayout(this).apply {
            visibility = View.GONE
            setBackgroundColor(0x88050B12.toInt())
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            )
            setOnClickListener { hideProcessPicker() }
        }
        val card = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.argb(238, 12, 21, 29))
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.CENTER_HORIZONTAL,
            ).apply {
                topMargin = (92f * density).roundToInt()
                marginStart = (14f * density).roundToInt()
                marginEnd = (14f * density).roundToInt()
            }
            setPadding((14f * density).roundToInt(), (14f * density).roundToInt(), (14f * density).roundToInt(), (14f * density).roundToInt())
            isClickable = true
        }
        card.setOnClickListener { }

        val title = TextView(this).apply {
            text = getString(com.smlc666.lkmdbg.R.string.process_panel_title)
            textSize = 16f
            setTextColor(Color.WHITE)
        }
        val filterRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(0, (10f * density).roundToInt(), 0, 0)
        }
        filterRow.addView(makeFilterButton(com.smlc666.lkmdbg.R.string.process_filter_all, ProcessFilter.All))
        filterRow.addView(makeFilterButton(com.smlc666.lkmdbg.R.string.process_filter_android, ProcessFilter.AndroidApps))
        filterRow.addView(makeFilterButton(com.smlc666.lkmdbg.R.string.process_filter_cmdline, ProcessFilter.CommandLine))
        filterRow.addView(makeFilterButton(com.smlc666.lkmdbg.R.string.process_filter_user, ProcessFilter.UserApps))

        val summary = TextView(this).apply {
            setPadding(0, (10f * density).roundToInt(), 0, (8f * density).roundToInt())
            textSize = 12f
            setTextColor(Color.argb(230, 205, 226, 240))
        }
        val scrollView = ScrollView(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                (320f * density).roundToInt(),
            )
        }
        val processList = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
        }
        scrollView.addView(processList)

        card.addView(title)
        card.addView(filterRow)
        card.addView(summary)
        card.addView(scrollView)
        container.addView(card)
        processPickerSummaryView = summary
        processPickerListView = processList
        return container
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

    private fun makeFilterButton(textRes: Int, filter: ProcessFilter): Button =
        Button(this).apply {
            text = getString(textRes)
            textSize = 11f
            layoutParams = LinearLayout.LayoutParams(
                0,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                1f,
            ).apply {
                marginEnd = (6f * resources.displayMetrics.density).roundToInt()
            }
            setOnClickListener {
                repository.updateProcessFilter(filter)
                renderProcessPicker(repository.state.value)
            }
        }

    private fun toggleProcessPicker() {
        val picker = processPickerView ?: return
        picker.visibility = if (picker.visibility == View.VISIBLE) View.GONE else View.VISIBLE
        if (picker.visibility == View.VISIBLE)
            renderProcessPicker(repository.state.value)
    }

    private fun hideProcessPicker() {
        processPickerView?.visibility = View.GONE
    }

    private fun renderProcessPicker(state: com.smlc666.lkmdbg.data.SessionBridgeState) {
        val summaryView = processPickerSummaryView ?: return
        val listView = processPickerListView ?: return
        val filtered = state.processes.filter { state.processFilter.matches(it) }
        summaryView.text = getString(
            com.smlc666.lkmdbg.R.string.process_summary_counts,
            state.processes.size,
            state.processes.count { it.isAndroidApp },
            state.processes.count { !it.isAndroidApp },
            state.processes.count { it.isAndroidApp && it.isSystemApp },
            state.processes.count { it.isAndroidApp && !it.isSystemApp },
        )
        listView.removeAllViews()
        if (filtered.isEmpty()) {
            listView.addView(
                TextView(this).apply {
                    text = getString(com.smlc666.lkmdbg.R.string.process_filter_empty)
                    setTextColor(Color.WHITE)
                    textSize = 13f
                },
            )
            return
        }
        filtered.take(24).forEach { process ->
            listView.addView(makeProcessRow(process))
        }
    }

    private fun makeProcessRow(process: ResolvedProcessRecord): View {
        val density = resources.displayMetrics.density
        return LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            ).apply {
                bottomMargin = (6f * density).roundToInt()
            }
            setPadding(
                (10f * density).roundToInt(),
                (8f * density).roundToInt(),
                (10f * density).roundToInt(),
                (8f * density).roundToInt(),
            )
            setBackgroundColor(Color.argb(210, 18, 32, 44))
            val iconView = ImageView(this@LkmdbgOverlayService).apply {
                layoutParams = LinearLayout.LayoutParams(
                    (28f * density).roundToInt(),
                    (28f * density).roundToInt(),
                ).apply {
                    marginEnd = (10f * density).roundToInt()
                    gravity = Gravity.CENTER_VERTICAL
                }
                iconLoader.load(process.iconPackageName)?.let(::setImageDrawable)
            }
            val textColumn = LinearLayout(this@LkmdbgOverlayService).apply {
                orientation = LinearLayout.VERTICAL
                layoutParams = LinearLayout.LayoutParams(
                    0,
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                    1f,
                )
            }
            val titleView = TextView(this@LkmdbgOverlayService).apply {
                text = process.displayName
                textSize = 13f
                setTextColor(Color.WHITE)
            }
            val subtitleView = TextView(this@LkmdbgOverlayService).apply {
                text = buildString {
                    append("pid=")
                    append(process.pid)
                    append(" uid=")
                    append(process.uid)
                    if (!process.packageName.isNullOrBlank()) {
                        append("  ")
                        append(process.packageName)
                    }
                }
                textSize = 11f
                setTextColor(Color.argb(230, 197, 214, 225))
            }
            val kindView = TextView(this@LkmdbgOverlayService).apply {
                text = when {
                    process.isAndroidApp && process.isSystemApp -> getString(com.smlc666.lkmdbg.R.string.process_kind_system_app)
                    process.isAndroidApp -> getString(com.smlc666.lkmdbg.R.string.process_kind_user_app)
                    else -> getString(com.smlc666.lkmdbg.R.string.process_kind_command_line)
                }
                textSize = 11f
                setTextColor(Color.argb(255, 101, 222, 215))
            }
            textColumn.addView(titleView)
            textColumn.addView(subtitleView)
            addView(iconView)
            addView(textColumn)
            addView(kindView)
            setOnClickListener {
                lifecycleScope.launch {
                    repository.attachProcess(process.pid)
                    hideProcessPicker()
                }
            }
        }
    }

    private var lastTouchRawX = 0f
    private var lastTouchRawY = 0f
    private var dragDistancePx = 0f

    private fun moveCollapsedBall(deltaX: Float, deltaY: Float) {
        if (expanded)
            return
        collapsedX += deltaX.roundToInt()
        collapsedY += deltaY.roundToInt()
        val view = rootView ?: return
        val params = layoutParams ?: return
        params.x = collapsedX
        params.y = collapsedY
        windowManager.updateViewLayout(view, params)
    }

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

    private fun createLayoutParams(expanded: Boolean): WindowManager.LayoutParams =
        WindowManager.LayoutParams(
            if (expanded) WindowManager.LayoutParams.MATCH_PARENT else collapsedDiameterPx,
            if (expanded) WindowManager.LayoutParams.MATCH_PARENT else collapsedDiameterPx,
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            if (expanded) expandedWindowFlags() else collapsedWindowFlags(),
            PixelFormat.TRANSLUCENT,
        ).apply {
            gravity = Gravity.TOP or Gravity.START
            x = if (expanded) 0 else collapsedX
            y = if (expanded) 0 else collapsedY
        }

    private fun collapsedWindowFlags(): Int =
        WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN or
            WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE

    private fun expandedWindowFlags(): Int =
        WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN

    private fun removeOverlay() {
        overlayJob?.cancel()
        rootView?.let { view ->
            runCatching { windowManager.removeView(view) }
        }
        rootView = null
        workspaceView = null
        statusView = null
        processPickerView = null
        processPickerSummaryView = null
        processPickerListView = null
        layoutParams = null
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
