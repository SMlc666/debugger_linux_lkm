package com.smlc666.lkmdbg.data

import android.content.Context
import com.smlc666.lkmdbg.appdata.R
import com.smlc666.lkmdbg.data.bridge.SessionBridgeClient
import com.smlc666.lkmdbg.shared.BridgeContinueTargetRequest
import com.smlc666.lkmdbg.shared.BridgeEventRecord
import com.smlc666.lkmdbg.shared.BridgeFreezeThreadsRequest
import com.smlc666.lkmdbg.shared.BridgeHwpointRequest
import com.smlc666.lkmdbg.shared.BridgeSingleStepRequest
import com.smlc666.lkmdbg.shared.BridgeStopState
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update

private val MEMORY_SELECTION_SIZES = listOf(1, 2, 4, 8, 16)

class SessionBridgeRepository(
    context: Context,
    private val client: PipeAgentClient = PipeAgentClient(context.applicationContext),
) {
    companion object {
        private const val MEMORY_BROWSER_PAGE_SIZE: UInt = 256u
        private const val MEMORY_ROW_BYTES = 16
        private const val MEMORY_SEARCH_SNAPSHOT_CHUNK_SIZE: UInt = 262144u
    }

    private val appContext = context.applicationContext
    private val processResolver = AndroidProcessResolver(appContext)
    private val searchEngine = MemorySearchEngine(
        object : MemorySearchEngine.Backend {
            override suspend fun search(
                regionPreset: UInt,
                maxResults: UInt,
                pattern: ByteArray,
            ) = client.searchMemory(regionPreset, maxResults, pattern)
        },
    )
    private val memoryPreviewBuilder = MemoryPreviewBuilder(
        pageSize = MEMORY_BROWSER_PAGE_SIZE,
        rowBytes = MEMORY_ROW_BYTES,
    )
    private val _state = MutableStateFlow(
        SessionBridgeState(
            agentPath = client.agentPathHint,
            lastMessage = appContext.getString(R.string.session_message_idle),
        ),
    )
    private val memorySearchSnapshotController = MemorySearchSnapshotController(
        cacheDir = appContext.cacheDir,
        snapshotChunkSize = MEMORY_SEARCH_SNAPSHOT_CHUNK_SIZE,
    )
    private val memoryEditor = MemoryEditorController(
        context = appContext,
        client = client,
        stateFlow = _state,
        memoryPreviewBuilder = memoryPreviewBuilder,
        pageSize = MEMORY_BROWSER_PAGE_SIZE,
    )
    private val operationRunner = SessionOperationRunner(
        context = appContext,
        stateFlow = _state,
    )
    private val memorySearchCoordinator = MemorySearchCoordinator(
        context = appContext,
        client = client,
        stateFlow = _state,
        searchEngine = searchEngine,
        snapshotController = memorySearchSnapshotController,
    )
    private val threadController = SessionThreadController(
        context = appContext,
        client = client,
        stateFlow = _state,
    )
    private val unsafeOps = SessionBridgeUnsafeOps(
        context = appContext,
        client = client,
        stateFlow = _state,
        processResolver = processResolver,
        memoryPreviewBuilder = memoryPreviewBuilder,
        discardMemorySearchSnapshot = { memorySearchCoordinator.discardSnapshot() },
    )
    private val sessionBridgeClient: SessionBridgeClient = client.asSessionBridgeClient()
    val state: StateFlow<SessionBridgeState> = _state.asStateFlow()

    fun rootBridgeDiagnostics(): RootBridgeDiagnostics = client.diagnostics()

    fun bridgeClient(): SessionBridgeClient = sessionBridgeClient

    fun updateTargetPidInput(value: String) {
        _state.update { current ->
            current.copy(targetPidInput = value.filter { it.isDigit() })
        }
    }

    fun updateTargetTidInput(value: String) {
        _state.update { current ->
            current.copy(targetTidInput = value.filter { it.isDigit() })
        }
    }

    fun updateWorkspaceSection(section: WorkspaceSection) {
        _state.update { current ->
            current.copy(
                workspaceSection = section,
            )
        }
    }

    fun updateProcessFilter(filter: ProcessFilter) {
        _state.update { current -> current.copy(processFilter = filter) }
    }

    fun selectProcess(pid: Int) {
        _state.update { current -> current.copy(selectedProcessPid = pid) }
    }

    fun cycleProcessFilter() {
        val values = ProcessFilter.entries
        _state.update { current ->
            current.copy(processFilter = values[(current.processFilter.ordinal + 1) % values.size])
        }
    }

    fun updateMemorySearchQuery(value: String) {
        _state.update { current ->
            current.copy(
                memorySearch = current.memorySearch.copy(query = value),
            )
        }
    }

    fun updateMemoryAddressInput(value: String) {
        val filtered = value.filter { it.isDigit() || it.lowercaseChar() in 'a'..'f' || it == 'x' || it == 'X' }
        _state.update { current -> current.copy(memoryAddressInput = filtered) }
    }

    fun updateMemorySelectionSize(size: Int) {
        if (size !in MEMORY_SELECTION_SIZES)
            return
        _state.update { current -> current.copy(memorySelectionSize = size) }
    }

    fun updateMemoryWriteHexInput(value: String) {
        val filtered = value.filter { it.isDigit() || it.lowercaseChar() in 'a'..'f' || it.isWhitespace() }
        _state.update { current -> current.copy(memoryWriteHexInput = filtered) }
    }

    fun updateMemoryWriteAsciiInput(value: String) {
        _state.update { current -> current.copy(memoryWriteAsciiInput = value) }
    }

    fun updateMemoryWriteAsmInput(value: String) {
        _state.update { current -> current.copy(memoryWriteAsmInput = value) }
    }

    fun updateMemorySearchValueType(valueType: MemorySearchValueType) {
        _state.update { current ->
            current.copy(
                memorySearch = current.memorySearch.copy(
                    valueType = valueType,
                    summary = "",
                    results = emptyList(),
                ),
            )
        }
    }

    fun cycleMemorySearchValueType() {
        val values = MemorySearchValueType.entries
        updateMemorySearchValueType(
            values[(state.value.memorySearch.valueType.ordinal + 1) % values.size],
        )
    }

    fun updateMemorySearchRefineMode(refineMode: MemorySearchRefineMode) {
        _state.update { current ->
            current.copy(
                memorySearch = current.memorySearch.copy(
                    refineMode = refineMode,
                    summary = "",
                    results = emptyList(),
                ),
            )
        }
    }

    fun cycleMemorySearchRefineMode() {
        val values = MemorySearchRefineMode.entries
        updateMemorySearchRefineMode(
            values[(state.value.memorySearch.refineMode.ordinal + 1) % values.size],
        )
    }

    fun updateMemoryRegionPreset(regionPreset: MemoryRegionPreset) {
        memorySearchCoordinator.discardSnapshot()
        _state.update { current ->
            current.copy(
                memorySearch = current.memorySearch.copy(
                    regionPreset = regionPreset,
                    summary = "",
                    results = emptyList(),
                ),
            )
        }
    }

    fun cycleMemoryRegionPreset() {
        val values = MemoryRegionPreset.entries
        updateMemoryRegionPreset(
            values[(state.value.memorySearch.regionPreset.ordinal + 1) % values.size],
        )
    }

    fun updateHwpointAddressInput(value: String) {
        val filtered = value.filter { it.isDigit() || it.lowercaseChar() in 'a'..'f' || it == 'x' || it == 'X' }
        _state.update { current -> current.copy(hwpointAddressInput = filtered) }
    }

    fun updateHwpointLengthInput(value: String) {
        _state.update { current -> current.copy(hwpointLengthInput = value.filter { it.isDigit() }) }
    }

    fun cycleHwpointPreset() {
        val values = HwpointPreset.entries
        _state.update { current ->
            val next = values[(current.hwpointPreset.ordinal + 1) % values.size]
            current.copy(
                hwpointPreset = next,
                hwpointLengthInput = next.defaultLength.toString(),
            )
        }
    }

    fun selectHwpoint(id: ULong) {
        _state.update { current -> current.copy(selectedHwpointId = id) }
    }

    fun updateEventsAutoPollEnabled(enabled: Boolean) {
        _state.update { current -> current.copy(eventsAutoPollEnabled = enabled) }
    }

    fun togglePinnedEvent(seq: ULong) {
        _state.update { current ->
            if (current.recentEvents.none { it.record.seq == seq })
                return@update current
            val next =
                if (seq in current.pinnedEventSeqs) {
                    current.pinnedEventSeqs - seq
                } else {
                    current.pinnedEventSeqs + seq
                }
            current.copy(pinnedEventSeqs = next)
        }
    }

    fun clearRecentEvents() {
        _state.update { current ->
            current.copy(
                recentEvents = emptyList(),
                pinnedEventSeqs = emptySet(),
            )
        }
    }

    suspend fun connect() {
        operationRunner.run { unsafeOps.connect() }
    }

    suspend fun openSession() {
        operationRunner.run {
            if (state.value.hello == null)
                unsafeOps.connect()
            unsafeOps.openSession()
            unsafeOps.refreshStatus()
            refreshStopStateUnsafe(updateMessage = false)
            refreshHwpointsUnsafe(updateMessage = false)
            if (state.value.processes.isEmpty())
                unsafeOps.refreshProcesses()
        }
    }

    suspend fun refreshStatus() {
        operationRunner.run {
            unsafeOps.refreshStatus()
            refreshStopStateUnsafe(updateMessage = false)
        }
    }

    suspend fun attachTarget() {
        val targetPid = state.value.targetPidInput.toIntOrNull()
        val targetTid = state.value.targetTidInput.toIntOrNull() ?: 0
        if (targetPid == null || targetPid <= 0) {
            _state.update { current ->
                current.copy(lastMessage = appContext.getString(R.string.session_error_invalid_pid))
            }
            return
        }

        operationRunner.run {
            attachTargetUnsafe(targetPid, targetTid)
        }
    }

    suspend fun refreshProcesses() {
        operationRunner.run {
            unsafeOps.refreshProcesses()
        }
    }

    suspend fun refreshThreads() {
        operationRunner.run {
            if (!hasActiveTarget()) {
                threadController.clearSelection()
                operationRunner.updateMessage(appContext.getString(R.string.thread_error_no_target))
                return@run
            }
            unsafeOps.refreshThreads()
        }
    }

    suspend fun refreshThreadRegisters(tid: Int) {
        operationRunner.run { threadController.refreshRegisters(tid) }
    }

    suspend fun selectThread(tid: Int) {
        refreshThreadRegisters(tid)
    }

    suspend fun refreshEvents(timeoutMs: Int = 0, maxEvents: Int = 16) {
        operationRunner.run {
            if (!state.value.snapshot.sessionOpen) {
                operationRunner.updateMessage(appContext.getString(R.string.event_error_no_session))
                return@run
            }
            unsafeOps.refreshEvents(timeoutMs, maxEvents)
        }
    }

    suspend fun refreshEventsBackground(timeoutMs: Int = 250, maxEvents: Int = 16) {
        if (!state.value.snapshot.sessionOpen || state.value.busy)
            return
        runCatching {
            val reply = client.pollEvents(timeoutMs, maxEvents)
            ensureBridgeStatusOk(reply.status, reply.message, "POLL_EVENT")
            mergeRecentEvents(reply.events, updateMessage = false)
        }.onFailure {
            updateEventsAutoPollEnabled(false)
        }
    }

    suspend fun refreshImages() {
        operationRunner.run {
            if (!hasActiveTarget()) {
                _state.update { current -> current.copy(images = emptyList()) }
                operationRunner.updateMessage(appContext.getString(R.string.memory_error_no_target))
                return@run
            }
            unsafeOps.refreshImages()
        }
    }

    suspend fun refreshVmas() {
        operationRunner.run {
            if (!hasActiveTarget()) {
                _state.update { current -> current.copy(vmas = emptyList()) }
                operationRunner.updateMessage(appContext.getString(R.string.memory_error_no_target))
                return@run
            }
            unsafeOps.refreshVmas()
        }
    }

    suspend fun refreshStopState() {
        operationRunner.run {
            if (!state.value.snapshot.sessionOpen) {
                _state.update { current -> current.copy(stopState = null) }
                operationRunner.updateMessage(appContext.getString(R.string.event_error_no_session))
                return@run
            }
            refreshStopStateUnsafe(updateMessage = true)
        }
    }

    suspend fun refreshHwpoints() {
        operationRunner.run {
            if (!state.value.snapshot.sessionOpen) {
                _state.update { current -> current.copy(hwpoints = emptyList(), selectedHwpointId = null) }
                operationRunner.updateMessage(appContext.getString(R.string.event_error_no_session))
                return@run
            }
            refreshHwpointsUnsafe(updateMessage = true)
        }
    }

    suspend fun runMemorySearch() {
        operationRunner.run {
            if (!hasActiveTarget()) {
                operationRunner.updateMessage(appContext.getString(R.string.memory_error_no_target))
                return@run
            }
            memorySearchCoordinator.runSearch(refreshVmas = { unsafeOps.refreshVmas() })
        }
    }

    suspend fun refineMemorySearch() {
        operationRunner.run {
            if (!hasActiveTarget()) {
                operationRunner.updateMessage(appContext.getString(R.string.memory_error_no_target))
                return@run
            }
            memorySearchCoordinator.refineSearch(refreshVmas = { unsafeOps.refreshVmas() })
        }
    }

    suspend fun openMemoryPage(remoteAddr: ULong) {
        operationRunner.run {
            memoryEditor.openPage(remoteAddr)
        }
    }

    suspend fun previewSelectedPc() {
        operationRunner.run {
            val pc = state.value.selectedThreadRegisters?.pc
                ?: state.value.threads.firstOrNull()?.userPc
            if (pc == null || pc == 0uL) {
                operationRunner.updateMessage(appContext.getString(R.string.memory_error_no_pc))
                return@run
            }
            memoryEditor.openPage(pc)
        }
    }

    suspend fun jumpToMemoryAddress() {
        val address = memoryEditor.parseAddressInput(state.value.memoryAddressInput)
        if (address == null) {
            operationRunner.updateMessage(appContext.getString(R.string.memory_error_invalid_address))
            return
        }
        openMemoryPage(address)
    }

    suspend fun stepMemoryPage(direction: Int) {
        operationRunner.run {
            val current = state.value.memoryPage
            if (current == null) {
                operationRunner.updateMessage(appContext.getString(R.string.memory_error_no_page))
                return@run
            }
            if (direction == 0)
                return@run

            val delta = current.pageSize.toULong()
            val nextFocus = if (direction < 0) {
                if (current.pageStart < delta)
                    0uL
                else
                    current.pageStart - delta
            } else {
                current.pageStart + delta
            }
            memoryEditor.openPage(nextFocus)
        }
    }

    suspend fun loadSelectionIntoHexSearch() {
        operationRunner.run {
            memoryEditor.loadSelectionIntoHexSearch()
        }
    }

    suspend fun loadSelectionIntoAsciiSearch() {
        operationRunner.run {
            memoryEditor.loadSelectionIntoAsciiSearch()
        }
    }

    suspend fun loadSelectionIntoEditors() {
        operationRunner.run {
            memoryEditor.loadSelectionIntoEditors()
        }
    }

    suspend fun assembleArm64ToEditors() {
        operationRunner.run {
            memoryEditor.assembleArm64ToEditors()
        }
    }

    suspend fun assembleArm64AndWrite() {
        operationRunner.run {
            memoryEditor.assembleArm64AndWrite()
        }
    }

    suspend fun writeHexAtFocus() {
        operationRunner.run {
            memoryEditor.writeHexAtFocus()
        }
    }

    suspend fun writeAsciiAtFocus() {
        operationRunner.run {
            memoryEditor.writeAsciiAtFocus()
        }
    }

    suspend fun selectMemoryAddress(remoteAddr: ULong) {
        operationRunner.run {
            memoryEditor.selectAddress(remoteAddr)
        }
    }

    suspend fun useSelectedPcForHwpoint() {
        operationRunner.run {
            val pc = state.value.selectedThreadRegisters?.pc ?: state.value.threads.firstOrNull()?.userPc
            if (pc == null || pc == 0uL)
                throw IllegalStateException(appContext.getString(R.string.memory_error_no_pc))
            _state.update { current -> current.copy(hwpointAddressInput = hex64(pc)) }
        }
    }

    suspend fun useMemoryFocusForHwpoint() {
        operationRunner.run {
            val focus = state.value.memoryPage?.focusAddress
                ?: throw IllegalStateException(appContext.getString(R.string.memory_error_no_page))
            _state.update { current -> current.copy(hwpointAddressInput = hex64(focus)) }
        }
    }

    suspend fun freezeThreads(timeoutMs: Int = 1500) {
        runSessionControl {
            ensureSessionReadyUnsafe()
            if (!hasActiveTarget())
                throw IllegalStateException(appContext.getString(R.string.thread_error_no_target))
            val reply = client.freezeThreads(BridgeFreezeThreadsRequest(timeoutMs = timeoutMs))
            ensureBridgeStatusOk(reply.status, reply.message, "FREEZE_THREADS")
            refreshStopStateUnsafe(updateMessage = false)
            _state.update { current ->
                current.copy(
                    lastMessage = reply.message.ifBlank {
                        appContext.getString(
                            R.string.session_message_threads_frozen,
                            reply.threadsParked.toString(),
                            reply.threadsTotal.toString(),
                        )
                    },
                )
            }
        }
    }

    suspend fun thawThreads(timeoutMs: Int = 1500) {
        runSessionControl {
            ensureSessionReadyUnsafe()
            if (!hasActiveTarget())
                throw IllegalStateException(appContext.getString(R.string.thread_error_no_target))
            val reply = client.thawThreads(BridgeFreezeThreadsRequest(timeoutMs = timeoutMs))
            ensureBridgeStatusOk(reply.status, reply.message, "THAW_THREADS")
            refreshStopStateUnsafe(updateMessage = false)
            _state.update { current ->
                current.copy(
                    lastMessage = reply.message.ifBlank {
                        appContext.getString(R.string.session_message_threads_thawed)
                    },
                )
            }
        }
    }

    suspend fun continueTarget(timeoutMs: Int = 1500) {
        runSessionControl {
            ensureSessionReadyUnsafe()
            if (!hasActiveTarget())
                throw IllegalStateException(appContext.getString(R.string.thread_error_no_target))
            val reply = client.continueTarget(
                BridgeContinueTargetRequest(
                    stopCookie = state.value.stopState?.cookie ?: 0uL,
                    timeoutMs = timeoutMs,
                ),
            )
            ensureBridgeStatusOk(reply.status, reply.message, "CONTINUE_TARGET")
            refreshStopStateUnsafe(updateMessage = false)
            refreshThreadsPreservingSelection(updateMessage = false)
            _state.update { current ->
                current.copy(
                    lastMessage = reply.message.ifBlank {
                        appContext.getString(R.string.session_message_target_continued)
                    },
                )
            }
        }
    }

    suspend fun singleStepSelectedThread() {
        runSessionControl {
            ensureSessionReadyUnsafe()
            val tid = currentSelectedTid()
            if (tid <= 0)
                throw IllegalStateException(appContext.getString(R.string.thread_error_no_selection))
            val reply = client.singleStep(BridgeSingleStepRequest(tid = tid))
            ensureBridgeStatusOk(reply.status, reply.message, "SINGLE_STEP")
            refreshStopStateUnsafe(updateMessage = false)
            _state.update { current ->
                current.copy(
                    lastMessage = reply.message.ifBlank {
                        appContext.getString(R.string.session_message_single_step, tid)
                    },
                )
            }
        }
    }

    suspend fun addHwpoint() {
        runSessionControl {
            ensureSessionReadyUnsafe()
            if (!hasActiveTarget())
                throw IllegalStateException(appContext.getString(R.string.thread_error_no_target))
            val request = buildHwpointRequest()
            val reply = client.addHwpoint(request)
            ensureBridgeStatusOk(reply.status, reply.message, "ADD_HWPOINT")
            refreshHwpointsUnsafe(updateMessage = false)
            _state.update { current ->
                current.copy(
                    selectedHwpointId = reply.id.takeIf { it != 0uL } ?: current.selectedHwpointId,
                    lastMessage = reply.message.ifBlank {
                        appContext.getString(R.string.hwpoint_message_added, hex64(reply.addr))
                    },
                )
            }
        }
    }

    suspend fun removeSelectedHwpoint() {
        runSessionControl {
            ensureSessionReadyUnsafe()
            val selected = state.value.hwpoints.firstOrNull { it.id == state.value.selectedHwpointId }
                ?: throw IllegalStateException(appContext.getString(R.string.hwpoint_error_no_selection))
            val reply = client.removeHwpoint(
                BridgeHwpointRequest(
                    id = selected.id,
                    addr = selected.addr,
                    tid = selected.tid,
                    type = selected.type,
                    len = selected.len,
                    flags = selected.flags,
                    triggerHitCount = selected.triggerHitCount,
                    actionFlags = selected.actionFlags,
                ),
            )
            ensureBridgeStatusOk(reply.status, reply.message, "REMOVE_HWPOINT")
            refreshHwpointsUnsafe(updateMessage = false)
            _state.update { current ->
                current.copy(
                    selectedHwpointId = current.hwpoints.firstOrNull { it.id != selected.id }?.id,
                    lastMessage = reply.message.ifBlank {
                        appContext.getString(R.string.hwpoint_message_removed, selected.id.toString())
                    },
                )
            }
        }
    }

    suspend fun attachProcess(targetPid: Int): Boolean =
        operationRunner.run {
            _state.update { current ->
                current.copy(
                    selectedProcessPid = targetPid,
                    targetPidInput = targetPid.toString(),
                    targetTidInput = "",
                )
            }
            attachTargetUnsafe(targetPid, 0)
            true
        } ?: false

    private fun hasActiveTarget(): Boolean = state.value.snapshot.targetPid > 0

    private suspend fun ensureSessionReadyUnsafe() {
        if (state.value.hello == null)
            unsafeOps.connect()
        if (!state.value.snapshot.sessionOpen)
            unsafeOps.openSession()
        unsafeOps.refreshStatus()
    }

    private suspend fun attachTargetUnsafe(targetPid: Int, targetTid: Int) {
        ensureSessionReadyUnsafe()
        unsafeOps.attachTarget(targetPid, targetTid)
        refreshThreadsPreservingSelection(updateMessage = false)
        refreshStopStateUnsafe(updateMessage = false)
        refreshHwpointsUnsafe(updateMessage = false)
        unsafeOps.refreshVmas()
        unsafeOps.refreshImages()
        unsafeOps.refreshEvents(timeoutMs = 0, maxEvents = 16)
        _state.update { current ->
            current.copy(
                selectedProcessPid = targetPid,
                targetPidInput = targetPid.toString(),
                targetTidInput = targetTid.takeIf { it > 0 }?.toString().orEmpty(),
            )
        }
    }

    private suspend fun refreshThreadsPreservingSelection(updateMessage: Boolean) {
        unsafeOps.refreshThreads(preferredTid = state.value.selectedThreadTid, updateMessage = updateMessage)
    }

    private suspend fun refreshStopStateUnsafe(updateMessage: Boolean) {
        val reply = client.getStopState()
        ensureBridgeStatusOk(reply.status, reply.message, "GET_STOP_STATE")
        val normalized = normalizeStopState(reply.stop)
        _state.update { current ->
            current.copy(
                stopState = normalized,
                lastMessage = if (updateMessage) {
                    reply.message.ifBlank {
                        if (normalized == null) {
                            appContext.getString(R.string.session_stop_state_idle)
                        } else {
                            appContext.getString(R.string.session_stop_state_ready, normalized.tid)
                        }
                    }
                } else {
                    current.lastMessage
                },
            )
        }
    }

    private suspend fun refreshHwpointsUnsafe(updateMessage: Boolean) {
        val reply = client.queryHwpoints()
        ensureBridgeStatusOk(reply.status, reply.message, "QUERY_HWPOINTS")
        _state.update { current ->
            val selected = current.selectedHwpointId?.takeIf { selectedId ->
                reply.hwpoints.any { it.id == selectedId }
            } ?: reply.hwpoints.firstOrNull()?.id
            current.copy(
                hwpoints = reply.hwpoints,
                selectedHwpointId = selected,
                lastMessage = if (updateMessage) {
                    reply.message.ifBlank {
                        appContext.getString(R.string.hwpoint_message_listed, reply.hwpoints.size)
                    }
                } else {
                    current.lastMessage
                },
            )
        }
    }

    private fun mergeRecentEvents(events: List<BridgeEventRecord>, updateMessage: Boolean) {
        val now = System.currentTimeMillis()
        _state.update { current ->
            val merged = buildList {
                events.forEach { add(SessionEventEntry(it, now)) }
                current.recentEvents.forEach { existing ->
                    if (none { it.record.seq == existing.record.seq })
                        add(existing)
                }
            }.take(256)
            val survivingPins = current.pinnedEventSeqs.filterTo(linkedSetOf()) { pinnedSeq ->
                merged.any { it.record.seq == pinnedSeq }
            }
            current.copy(
                recentEvents = merged,
                pinnedEventSeqs = survivingPins,
                lastMessage = if (updateMessage && events.isNotEmpty()) {
                    appContext.getString(R.string.event_message_refreshed, events.size)
                } else {
                    current.lastMessage
                },
            )
        }
    }

    private fun normalizeStopState(stop: BridgeStopState): BridgeStopState? =
        if (stop.cookie == 0uL && stop.flags == 0u && stop.tid == 0 && stop.reason == 0u) {
            null
        } else {
            stop
        }

    private fun currentSelectedTid(): Int =
        state.value.selectedThreadTid
            ?: state.value.stopState?.tid
            ?: state.value.snapshot.targetTid

    private suspend fun runSessionControl(block: suspend () -> Unit) {
        _state.update { current -> current.copy(sessionControlsBusy = true) }
        try {
            operationRunner.run(block)
        } finally {
            _state.update { current -> current.copy(sessionControlsBusy = false) }
        }
    }

    private fun buildHwpointRequest(): BridgeHwpointRequest {
        val current = state.value
        val addr = memoryEditor.parseAddressInput(current.hwpointAddressInput)
            ?: throw IllegalStateException(appContext.getString(R.string.hwpoint_error_invalid_address))
        val len = current.hwpointLengthInput.toUIntOrNull()
            ?: current.hwpointPreset.defaultLength.toUInt()
        if (len == 0u)
            throw IllegalStateException(appContext.getString(R.string.hwpoint_error_invalid_length))
        return BridgeHwpointRequest(
            addr = addr,
            tid = current.selectedThreadTid ?: 0,
            type = current.hwpointPreset.type,
            len = len,
            flags = current.hwpointPreset.flags,
        )
    }
}
