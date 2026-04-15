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
import com.smlc666.lkmdbg.appdata.R
import com.smlc666.lkmdbg.data.SessionBridgeRepository
import com.smlc666.lkmdbg.data.WorkspaceSection
import com.smlc666.lkmdbg.data.gateway.EventGatewayImpl
import com.smlc666.lkmdbg.data.gateway.ThreadGatewayImpl
import com.smlc666.lkmdbg.domain.event.EventUseCases
import com.smlc666.lkmdbg.domain.thread.ThreadUseCases
import com.smlc666.lkmdbg.overlay.presentation.workspace.WorkspaceIntent
import com.smlc666.lkmdbg.overlay.presentation.workspace.WorkspaceUiState
import com.smlc666.lkmdbg.overlay.presentation.workspace.WorkspaceViewModel
import com.smlc666.lkmdbg.overlay.ui.screens.CollapsedWorkspaceScreen
import com.smlc666.lkmdbg.overlay.ui.screens.MainWorkspaceScreen
import com.smlc666.lkmdbg.overlay.ui.theme.LkmdbgTheme
import com.smlc666.lkmdbg.platform.overlay.OverlayHostController
import com.smlc666.lkmdbg.shell.AppIconLoader
import com.smlc666.lkmdbg.shell.SessionAutomationController
import kotlinx.coroutines.Job
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
    private lateinit var hostController: OverlayHostController
    private lateinit var overlaySavedStateOwner: OverlaySavedStateOwner
    private lateinit var workspaceViewModel: WorkspaceViewModel
    private var rootView: FrameLayout? = null
    private var workspaceView: ComposeView? = null
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
                    hostController.afterProcessPickerAction()
                }
            }
            hostController = OverlayHostController(
                repository = repository,
                processPickerController = processPickerController,
                scope = lifecycleScope,
            )
            workspaceViewModel = WorkspaceViewModel(
                initialState = WorkspaceUiState.initial(),
                threadUseCases = ThreadUseCases(ThreadGatewayImpl(repository)),
                eventUseCases = EventUseCases(EventGatewayImpl(repository)),
                scope = lifecycleScope,
            )
            stateBinder = OverlayStateBinder(
                repository = repository,
                onStateChanged = { _, state ->
                    hostController.onRepositoryStateChanged(state)
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
        // LifecycleService uses onStartCommand() to advance its Lifecycle to STARTED.
        // Without this, Compose's window recomposer may not run, so UI won't recompose
        // on StateFlow updates until the overlay view is recreated.
        super.onStartCommand(intent, flags, startId)
        if (intent?.action == ACTION_STOP) {
            stopSelf()
            return START_NOT_STICKY
        }
        return START_STICKY
    }

    override fun onDestroy() {
        overlayJob?.cancel()
        hostController.setExpanded(false)
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
        hostController.setExpanded(expanded)
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
                            onSectionSelected = { section ->
                                dispatchWorkspaceIntent(WorkspaceIntent.SelectSection(section))
                            },
                            onToggleProcessPicker = {
                                hostController.toggleProcessPicker()
                            },
                            onTargetPidInputChanged = repository::updateTargetPidInput,
                            onTargetTidInputChanged = repository::updateTargetTidInput,
                            onConnect = { lifecycleScope.launch { repository.connect() } },
                            onOpenSession = { lifecycleScope.launch { repository.openSession() } },
                            onRefreshSession = { lifecycleScope.launch { repository.refreshStatus() } },
                            onAttachTarget = {
                                lifecycleScope.launch {
                                    repository.attachTarget()
                                    hostController.selectSection(WorkspaceSection.Memory)
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
                                        hostController.selectSection(WorkspaceSection.Memory)
                                    }
                                }
                            },
                            onRefreshThreads = { lifecycleScope.launch { repository.refreshThreads() } },
                            onSelectThread = { tid ->
                                dispatchWorkspaceIntent(WorkspaceIntent.SelectThread(tid))
                            },
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
                                hostController.toggleEventsAutoPollEnabled()
                            },
                            onClearEvents = repository::clearRecentEvents,
                            onTogglePinnedEvent = { seq ->
                                dispatchWorkspaceIntent(WorkspaceIntent.TogglePinnedEvent(seq))
                            },
                            onOpenEventThread = { tid ->
                                dispatchWorkspaceIntent(WorkspaceIntent.SelectSection(WorkspaceSection.Threads))
                                dispatchWorkspaceIntent(WorkspaceIntent.SelectThread(tid))
                            },
                            onOpenEventValue = { value ->
                                lifecycleScope.launch {
                                    repository.selectMemoryAddress(value)
                                }
                            },
                            onStepMemoryPage = { direction ->
                                lifecycleScope.launch {
                                    repository.stepMemoryPage(direction)
                                }
                            },
                            onSelectMemoryAddress = { address ->
                                lifecycleScope.launch {
                                    repository.selectMemoryAddress(address)
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
                                }
                            },
                            onWriteAsciiAtFocus = {
                                lifecycleScope.launch {
                                    repository.writeAsciiAtFocus()
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
                                }
                            },
                            onRunMemorySearch = {
                                lifecycleScope.launch {
                                    repository.runMemorySearch()
                                }
                            },
                            onRefineMemorySearch = {
                                lifecycleScope.launch {
                                    repository.refineMemorySearch()
                                }
                            },
                            onRefreshVmas = { lifecycleScope.launch { repository.refreshVmas() } },
                            onRefreshImages = { lifecycleScope.launch { repository.refreshImages() } },
                            onPreviewSelectedPc = {
                                lifecycleScope.launch {
                                    repository.previewSelectedPc()
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
        hostController.onRepositoryStateChanged(repository.state.value)
    }

    private fun dispatchWorkspaceIntent(intent: WorkspaceIntent) {
        if (intent is WorkspaceIntent.SelectSection)
            hostController.selectSection(intent.section)
        workspaceViewModel.dispatch(intent)
    }

    private fun updateExpandedState(nextExpanded: Boolean) {
        if (expanded == nextExpanded)
            return
        overlayJob?.cancel()
        // Stop host-side background work and expensive views before tearing down.
        hostController.setExpanded(false)
        expanded = nextExpanded
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
        hostController.setExpanded(false)
        rootView?.let { view ->
            runCatching { windowManager.removeView(view) }
        }
        rootView = null
        workspaceView = null
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
