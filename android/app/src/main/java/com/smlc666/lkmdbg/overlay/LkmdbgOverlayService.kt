package com.smlc666.lkmdbg.overlay

import android.content.Context
import android.content.Intent
import android.os.IBinder
import android.view.ContextThemeWrapper
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.LinearLayout
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.platform.ViewCompositionStrategy
import androidx.lifecycle.LifecycleService
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import com.smlc666.lkmdbg.CrashLogger
import com.smlc666.lkmdbg.LkmdbgApplication
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.data.ProcessFilter
import com.smlc666.lkmdbg.data.SessionBridgeRepository
import com.smlc666.lkmdbg.data.SessionBridgeState
import com.smlc666.lkmdbg.data.WorkspaceSection
import com.smlc666.lkmdbg.overlay.ui.screens.CollapsedWorkspaceScreen
import com.smlc666.lkmdbg.overlay.ui.screens.MainWorkspaceScreen
import com.smlc666.lkmdbg.overlay.ui.theme.LkmdbgTheme
import com.smlc666.lkmdbg.shell.AppIconLoader
import com.smlc666.lkmdbg.shell.SessionAutomationController
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.util.concurrent.atomic.AtomicBoolean

class LkmdbgOverlayService : LifecycleService() {
    private lateinit var windowManager: WindowManager
    private lateinit var windowController: OverlayWindowController
    private lateinit var overlayContext: Context
    private lateinit var repository: SessionBridgeRepository
    private lateinit var automation: SessionAutomationController
    private lateinit var gestureController: OverlayGestureController
    private lateinit var processPickerController: OverlayProcessPickerController
    private lateinit var stateBinder: OverlayStateBinder
    private lateinit var overlaySavedStateOwner: OverlaySavedStateOwner
    private var rootView: FrameLayout? = null
    private var workspaceView: ComposeView? = null
    private var overlayJob: Job? = null
    private var eventAutoPollJob: Job? = null
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
            overlayContext = ContextThemeWrapper(this, R.style.Theme_Lkmdbg)
            overlaySavedStateOwner = OverlaySavedStateOwner(this)
            windowController = OverlayWindowController(resources, windowManager)
            gestureController = OverlayGestureController(
                clickSlopPx = 12f * resources.displayMetrics.density,
            )
            processPickerController = OverlayProcessPickerController(
                context = overlayContext,
                repository = repository,
                iconLoader = AppIconLoader(this),
            ) { action ->
                lifecycleScope.launch {
                    action()
                    if (repository.state.value.workspaceSection == WorkspaceSection.Memory) {
                        updateMemoryViewMode(0)
                        repository.updateMemoryToolsOpen(false)
                    }
                    renderOverlayState()
                }
            }
            stateBinder = OverlayStateBinder(
                repository = repository,
                onStateChanged = { state ->
                    renderOverlayState(state)
                },
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
        stopEventAutoPoll()
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
        val root = FrameLayout(overlayContext).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            )
        }
        val nativeView = ComposeView(overlayContext).apply {
            setViewCompositionStrategy(ViewCompositionStrategy.DisposeOnDetachedFromWindow)
            setContent {
                val state by repository.state.collectAsState()
                LkmdbgTheme {
                    if (expanded) {
                        MainWorkspaceScreen(
                            state = state,
                            memoryViewMode = state.memoryViewMode,
                            memoryToolsOpen = state.memoryToolsOpen,
                            onSectionSelected = { section ->
                                repository.updateWorkspaceSection(section)
                                handleSectionSelection(section)
                            },
                            onToggleProcessPicker = {
                                repository.updateWorkspaceSection(WorkspaceSection.Processes)
                                handleSectionSelection(WorkspaceSection.Processes)
                                processPickerController.toggle(repository.state.value)
                            },
                            onToggleMemoryTools = { toggleMemoryTools() },
                            onTargetPidInputChanged = repository::updateTargetPidInput,
                            onTargetTidInputChanged = repository::updateTargetTidInput,
                            onConnect = { lifecycleScope.launch { repository.connect() } },
                            onOpenSession = { lifecycleScope.launch { repository.openSession() } },
                            onRefreshSession = { lifecycleScope.launch { repository.refreshStatus() } },
                            onAttachTarget = {
                                lifecycleScope.launch {
                                    repository.attachTarget()
                                    repository.updateWorkspaceSection(WorkspaceSection.Memory)
                                }
                            },
                            onRefreshStopState = { lifecycleScope.launch { repository.refreshStopState() } },
                            onFreezeThreads = { lifecycleScope.launch { repository.freezeThreads() } },
                            onThawThreads = { lifecycleScope.launch { repository.thawThreads() } },
                            onContinueTarget = { lifecycleScope.launch { repository.continueTarget() } },
                            onSingleStep = { lifecycleScope.launch { repository.singleStepSelectedThread() } },
                            onRefreshProcesses = { lifecycleScope.launch { repository.refreshProcesses() } },
                            onProcessFilterSelected = repository::updateProcessFilter,
                            onSelectProcess = repository::selectProcess,
                            onAttachSelectedProcess = { pid ->
                                lifecycleScope.launch {
                                    if (repository.attachProcess(pid)) {
                                        repository.updateWorkspaceSection(WorkspaceSection.Memory)
                                        updateMemoryViewMode(0)
                                    }
                                }
                            },
                            onRefreshThreads = { lifecycleScope.launch { repository.refreshThreads() } },
                            onSelectThread = { tid -> lifecycleScope.launch { repository.selectThread(tid) } },
                            onRefreshSelectedThreadRegisters = {
                                lifecycleScope.launch {
                                    repository.state.value.selectedThreadTid?.let { repository.refreshThreadRegisters(it) }
                                        ?: repository.refreshThreads()
                                }
                            },
                            onHwpointAddressChanged = repository::updateHwpointAddressInput,
                            onHwpointLengthChanged = repository::updateHwpointLengthInput,
                            onCycleHwpointPreset = repository::cycleHwpointPreset,
                            onUseSelectedPcForHwpoint = { lifecycleScope.launch { repository.useSelectedPcForHwpoint() } },
                            onUseMemoryFocusForHwpoint = { lifecycleScope.launch { repository.useMemoryFocusForHwpoint() } },
                            onAddHwpoint = { lifecycleScope.launch { repository.addHwpoint() } },
                            onSelectHwpoint = repository::selectHwpoint,
                            onRemoveSelectedHwpoint = { lifecycleScope.launch { repository.removeSelectedHwpoint() } },
                            onRefreshHwpoints = { lifecycleScope.launch { repository.refreshHwpoints() } },
                            onRefreshEvents = { lifecycleScope.launch { repository.refreshEvents(timeoutMs = 100, maxEvents = 32) } },
                            onToggleEventsAutoPoll = {
                                repository.updateEventsAutoPollEnabled(!repository.state.value.eventsAutoPollEnabled)
                                renderOverlayState()
                            },
                            onClearEvents = repository::clearRecentEvents,
                            onTogglePinnedEvent = repository::togglePinnedEvent,
                            onOpenEventThread = { tid ->
                                lifecycleScope.launch {
                                    repository.updateWorkspaceSection(WorkspaceSection.Threads)
                                    handleSectionSelection(WorkspaceSection.Threads)
                                    repository.selectThread(tid)
                                }
                            },
                            onOpenEventValue = { value ->
                                lifecycleScope.launch {
                                    repository.updateWorkspaceSection(WorkspaceSection.Memory)
                                    handleSectionSelection(WorkspaceSection.Memory)
                                    repository.selectMemoryAddress(value)
                                    updateMemoryViewMode(0)
                                }
                            },
                            onStepMemoryPage = { direction ->
                                lifecycleScope.launch {
                                    repository.stepMemoryPage(direction)
                                    updateMemoryViewMode(0)
                                }
                            },
                            onSelectMemoryAddress = { address ->
                                lifecycleScope.launch {
                                    repository.selectMemoryAddress(address)
                                    updateMemoryViewMode(0)
                                }
                            },
                            onMemorySearchQueryChanged = repository::updateMemorySearchQuery,
                            onMemoryAddressInputChanged = repository::updateMemoryAddressInput,
                            onMemorySelectionSizeChanged = repository::updateMemorySelectionSize,
                            onMemoryWriteHexChanged = repository::updateMemoryWriteHexInput,
                            onMemoryWriteAsciiChanged = repository::updateMemoryWriteAsciiInput,
                            onMemoryWriteAsmChanged = repository::updateMemoryWriteAsmInput,
                            onCycleMemorySearchValueType = repository::cycleMemorySearchValueType,
                            onCycleMemorySearchRefineMode = repository::cycleMemorySearchRefineMode,
                            onCycleMemoryRegionPreset = repository::cycleMemoryRegionPreset,
                            onJumpMemoryAddress = {
                                lifecycleScope.launch {
                                    repository.jumpToMemoryAddress()
                                    updateMemoryViewMode(0)
                                }
                            },
                            onLoadSelectionIntoHexSearch = {
                                lifecycleScope.launch {
                                    repository.loadSelectionIntoHexSearch()
                                }
                            },
                            onLoadSelectionIntoAsciiSearch = {
                                lifecycleScope.launch {
                                    repository.loadSelectionIntoAsciiSearch()
                                }
                            },
                            onLoadSelectionIntoEditors = {
                                lifecycleScope.launch {
                                    repository.loadSelectionIntoEditors()
                                }
                            },
                            onWriteHexAtFocus = {
                                lifecycleScope.launch {
                                    repository.writeHexAtFocus()
                                    updateMemoryViewMode(0)
                                }
                            },
                            onWriteAsciiAtFocus = {
                                lifecycleScope.launch {
                                    repository.writeAsciiAtFocus()
                                    updateMemoryViewMode(0)
                                }
                            },
                            onAssembleToEditors = {
                                lifecycleScope.launch {
                                    repository.assembleArm64ToEditors()
                                }
                            },
                            onAssembleAndWrite = {
                                lifecycleScope.launch {
                                    repository.assembleArm64AndWrite()
                                    updateMemoryViewMode(0)
                                }
                            },
                            onRunMemorySearch = {
                                lifecycleScope.launch {
                                    repository.runMemorySearch()
                                    updateMemoryViewMode(1)
                                }
                            },
                            onRefineMemorySearch = {
                                lifecycleScope.launch {
                                    repository.refineMemorySearch()
                                    updateMemoryViewMode(1)
                                }
                            },
                            onRefreshVmas = { lifecycleScope.launch { repository.refreshVmas() } },
                            onRefreshImages = { lifecycleScope.launch { repository.refreshImages() } },
                            onShowMemoryResults = { updateMemoryViewMode(1) },
                            onShowMemoryPage = { updateMemoryViewMode(0) },
                            onPreviewSelectedPc = {
                                lifecycleScope.launch {
                                    repository.previewSelectedPc()
                                    updateMemoryViewMode(0)
                                }
                            },
                            onClose = { stopSelf() },
                            onCollapse = { updateExpandedState(false) },
                        )
                    } else {
                        CollapsedWorkspaceScreen()
                    }
                }
            }
            setOnTouchListener { _: View, event: MotionEvent ->
                if (expanded)
                    return@setOnTouchListener false
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

        if (expanded) {
            val column = LinearLayout(overlayContext).apply {
                orientation = LinearLayout.VERTICAL
                layoutParams = FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT,
                )
                setPadding(
                    (10f * density).toInt(),
                    (10f * density).toInt(),
                    (10f * density).toInt(),
                    (10f * density).toInt(),
                )
            }
            val body = FrameLayout(overlayContext).apply {
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT,
                )
            }
            nativeView.layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            )
            body.addView(nativeView)
            body.addView(
                processPickerController.build(density),
                FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT,
                ),
            )
            column.addView(body)
            root.addView(column)
        } else {
            nativeView.layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            )
            root.addView(nativeView)
        }

        workspaceView = nativeView
        rootView = root
        layoutParams = params
        root.setViewTreeLifecycleOwner(this)
        root.setViewTreeSavedStateRegistryOwner(overlaySavedStateOwner)
        windowManager.addView(root, params)
        overlayJob?.cancel()
        overlayJob = stateBinder.bind(lifecycleScope)
        renderOverlayState()
    }

    private fun updateExpandedState(nextExpanded: Boolean) {
        if (expanded == nextExpanded)
            return
        expanded = nextExpanded
        overlayJob?.cancel()
        stopEventAutoPoll()
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
        stopEventAutoPoll()
        rootView?.let { view ->
            runCatching { windowManager.removeView(view) }
        }
        rootView = null
        workspaceView = null
        layoutParams = null
        if (::processPickerController.isInitialized)
            processPickerController.clear()
    }

    private fun toggleMemoryTools() {
        if (repository.state.value.workspaceSection != WorkspaceSection.Memory)
            return
        repository.updateMemoryToolsOpen(!repository.state.value.memoryToolsOpen)
        renderOverlayState()
    }

    private fun updateMemoryViewMode(mode: Int) {
        if (repository.state.value.memoryViewMode == mode)
            return
        repository.updateMemoryViewMode(mode)
        renderOverlayState()
    }

    private fun handleSectionSelection(section: WorkspaceSection) {
        if (section == WorkspaceSection.Events) {
            repository.updateEventsAutoPollEnabled(true)
        } else {
            repository.updateEventsAutoPollEnabled(false)
        }
        if (section != WorkspaceSection.Processes)
            processPickerController.hide()
        renderOverlayState()
    }

    private fun renderOverlayState(state: SessionBridgeState = repository.state.value) {
        if (!expanded) {
            stopEventAutoPoll()
            return
        }
        if (state.workspaceSection != WorkspaceSection.Processes)
            processPickerController.hide()

        processPickerController.render(state)
        syncEventAutoPoll(state)
    }

    private fun syncEventAutoPoll(state: SessionBridgeState) {
        val shouldPoll =
            expanded &&
                state.workspaceSection == WorkspaceSection.Events &&
                state.eventsAutoPollEnabled &&
                state.snapshot.sessionOpen
        if (!shouldPoll) {
            stopEventAutoPoll()
            return
        }
        if (eventAutoPollJob?.isActive == true)
            return
        eventAutoPollJob = lifecycleScope.launch {
            while (isActive) {
                repository.refreshEventsBackground(timeoutMs = 100, maxEvents = 16)
                delay(400)
            }
        }
    }

    private fun stopEventAutoPoll() {
        eventAutoPollJob?.cancel()
        eventAutoPollJob = null
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
