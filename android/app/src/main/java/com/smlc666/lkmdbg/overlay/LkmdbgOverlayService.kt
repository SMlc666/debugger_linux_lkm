package com.smlc666.lkmdbg.overlay

import android.content.Context
import android.content.Intent
import android.os.IBinder
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.FrameLayout
import androidx.lifecycle.LifecycleService
import androidx.lifecycle.lifecycleScope
import com.smlc666.lkmdbg.CrashLogger
import com.smlc666.lkmdbg.LkmdbgApplication
import com.smlc666.lkmdbg.data.SessionBridgeRepository
import com.smlc666.lkmdbg.nativeui.NativeWorkspaceTextureView
import com.smlc666.lkmdbg.shell.AppIconLoader
import com.smlc666.lkmdbg.shell.SessionAutomationController
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import java.util.concurrent.atomic.AtomicBoolean

class LkmdbgOverlayService : LifecycleService() {
    private lateinit var windowManager: WindowManager
    private lateinit var windowController: OverlayWindowController
    private lateinit var repository: SessionBridgeRepository
    private lateinit var automation: SessionAutomationController
    private lateinit var headerController: OverlayHeaderController
    private lateinit var gestureController: OverlayGestureController
    private lateinit var processPickerController: OverlayProcessPickerController
    private lateinit var memoryToolboxController: OverlayMemoryToolboxController
    private lateinit var workspaceActionController: OverlayWorkspaceActionController
    private lateinit var stateBinder: OverlayStateBinder
    private var rootView: FrameLayout? = null
    private var workspaceView: NativeWorkspaceTextureView? = null
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
            gestureController = OverlayGestureController(
                clickSlopPx = 12f * resources.displayMetrics.density,
            )
            headerController = OverlayHeaderController(this) { action ->
                lifecycleScope.launch {
                    action()
                }
            }
            processPickerController = OverlayProcessPickerController(
                context = this,
                repository = repository,
                iconLoader = AppIconLoader(this),
            ) { action ->
                lifecycleScope.launch {
                    action()
                }
            }
            memoryToolboxController = OverlayMemoryToolboxController(
                context = this,
                repository = repository,
            ) { action ->
                lifecycleScope.launch {
                    action()
                }
            }
            workspaceActionController = OverlayWorkspaceActionController(
                repository = repository,
            ) { action ->
                lifecycleScope.launch {
                    action()
                }
            }
            stateBinder = OverlayStateBinder(
                context = this,
                repository = repository,
                headerController = headerController,
                processPickerController = processPickerController,
                memoryToolboxController = memoryToolboxController,
            )
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
            setOnWorkspaceActionListener { action ->
                workspaceActionController.dispatch(action)
            }
            setOnTouchListener { _: View, event: MotionEvent ->
                if (expanded)
                    return@setOnTouchListener dispatchNativeTouch(event)
                gestureController.onCollapsedTouch(
                    event = event,
                    moveBy = { dx, dy ->
                        val view = rootView
                        val currentParams = this@LkmdbgOverlayService.layoutParams
                        if (view != null && currentParams != null)
                            windowController.moveCollapsed(view, currentParams, dx, dy)
                    },
                    expand = { updateExpandedState(true) },
                )
            }
        }
        root.addView(nativeView)

        if (expanded) {
            root.addView(
                headerController.build(
                    density = density,
                    onCollapse = { updateExpandedState(false) },
                    onClose = { stopSelf() },
                    onConnect = { repository.connect() },
                    onOpenSession = { repository.openSession() },
                    onRefreshStatus = { repository.refreshStatus() },
                    onRefreshProcesses = { repository.refreshProcesses() },
                    onToggleProcessPicker = {
                        processPickerController.toggle(repository.state.value)
                    },
                    onRefreshEvents = { repository.refreshEvents(timeoutMs = 0, maxEvents = 16) },
                ),
            )
            root.addView(processPickerController.build(density))
            root.addView(memoryToolboxController.build(density))
        }

        workspaceView = nativeView
        rootView = root
        layoutParams = params
        windowManager.addView(root, params)
        overlayJob?.cancel()
        overlayJob = stateBinder.bind(
            scope = lifecycleScope,
            expanded = expanded,
            workspaceView = { workspaceView },
        )
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
        layoutParams = null
        if (::headerController.isInitialized)
            headerController.clear()
        if (::processPickerController.isInitialized)
            processPickerController.clear()
        if (::memoryToolboxController.isInitialized)
            memoryToolboxController.clear()
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
