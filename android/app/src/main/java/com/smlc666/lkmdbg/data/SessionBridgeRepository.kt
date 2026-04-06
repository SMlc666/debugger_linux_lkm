package com.smlc666.lkmdbg.data

import android.content.Context
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.shared.BridgeEventRecord
import com.smlc666.lkmdbg.shared.BridgeHelloReply
import com.smlc666.lkmdbg.shared.BridgeImageRecord
import com.smlc666.lkmdbg.shared.BridgeStatusSnapshot
import com.smlc666.lkmdbg.shared.BridgeThreadRecord
import com.smlc666.lkmdbg.shared.BridgeThreadRegistersReply
import com.smlc666.lkmdbg.shared.BridgeVmaRecord
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update

data class SessionEventEntry(
    val record: BridgeEventRecord,
    val receivedAtMs: Long,
)

private val MEMORY_SELECTION_SIZES = listOf(1, 2, 4, 8, 16)

data class MemorySearchUiState(
    val query: String = "",
    val valueType: MemorySearchValueType = MemorySearchValueType.Int32,
    val refineMode: MemorySearchRefineMode = MemorySearchRefineMode.Exact,
    val regionPreset: MemoryRegionPreset = MemoryRegionPreset.All,
    val snapshotReady: Boolean = false,
    val summary: String = "",
    val results: List<MemorySearchResult> = emptyList(),
)

data class SessionBridgeState(
    val busy: Boolean = false,
    val agentPath: String,
    val workspaceSection: WorkspaceSection = WorkspaceSection.Memory,
    val targetPidInput: String = "",
    val hello: BridgeHelloReply? = null,
    val snapshot: BridgeStatusSnapshot = BridgeStatusSnapshot(
        status = 0,
        connected = false,
        targetPid = 0,
        targetTid = 0,
        sessionOpen = false,
        agentPid = 0,
        ownerPid = 0,
        hookActive = 0,
        eventQueueDepth = 0u,
        sessionId = 0u,
        transport = "su->stdio-pipe",
        message = "idle",
    ),
    val lastMessage: String,
    val processFilter: ProcessFilter = ProcessFilter.All,
    val processes: List<ResolvedProcessRecord> = emptyList(),
    val threads: List<BridgeThreadRecord> = emptyList(),
    val selectedThreadTid: Int? = null,
    val selectedThreadRegisters: BridgeThreadRegistersReply? = null,
    val recentEvents: List<SessionEventEntry> = emptyList(),
    val images: List<BridgeImageRecord> = emptyList(),
    val vmas: List<BridgeVmaRecord> = emptyList(),
    val memoryAddressInput: String = "",
    val memorySelectionSize: Int = 4,
    val memoryWriteHexInput: String = "",
    val memoryWriteAsciiInput: String = "",
    val memoryWriteAsmInput: String = "",
    val memoryPage: MemoryPage? = null,
    val memorySearch: MemorySearchUiState = MemorySearchUiState(),
    val memoryToolsOpen: Boolean = false,
    val memoryViewMode: Int = 0,
)

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
    val state: StateFlow<SessionBridgeState> = _state.asStateFlow()

    fun rootBridgeDiagnostics(): RootBridgeDiagnostics = client.diagnostics()

    fun updateTargetPidInput(value: String) {
        _state.update { current ->
            current.copy(targetPidInput = value.filter { it.isDigit() })
        }
    }

    fun updateMemoryToolsOpen(open: Boolean) {
        _state.update { current -> current.copy(memoryToolsOpen = open) }
    }

    fun updateMemoryViewMode(mode: Int) {
        _state.update { current -> current.copy(memoryViewMode = mode) }
    }

    fun updateWorkspaceSection(section: WorkspaceSection) {
        _state.update { current ->
            current.copy(
                workspaceSection = section,
                memoryToolsOpen = if (section != WorkspaceSection.Memory) false else current.memoryToolsOpen,
                memoryViewMode = if (section != WorkspaceSection.Memory) 0 else current.memoryViewMode
            )
        }
    }

    fun updateProcessFilter(filter: ProcessFilter) {
        _state.update { current -> current.copy(processFilter = filter) }
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

    suspend fun connect() {
        operationRunner.run { unsafeOps.connect() }
    }

    suspend fun openSession() {
        operationRunner.run {
            if (state.value.hello == null)
                unsafeOps.connect()
            unsafeOps.openSession()
            unsafeOps.refreshStatus()
            if (state.value.processes.isEmpty())
                unsafeOps.refreshProcesses()
        }
    }

    suspend fun refreshStatus() {
        operationRunner.run { unsafeOps.refreshStatus() }
    }

    suspend fun attachTarget() {
        val targetPid = state.value.targetPidInput.toIntOrNull()
        if (targetPid == null || targetPid <= 0) {
            _state.update { current ->
                current.copy(lastMessage = appContext.getString(R.string.session_error_invalid_pid))
            }
            return
        }

        operationRunner.run {
            attachTargetUnsafe(targetPid)
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

    suspend fun refreshEvents(timeoutMs: Int = 0, maxEvents: Int = 16) {
        operationRunner.run {
            if (!state.value.snapshot.sessionOpen) {
                operationRunner.updateMessage(appContext.getString(R.string.event_error_no_session))
                return@run
            }
            unsafeOps.refreshEvents(timeoutMs, maxEvents)
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

    suspend fun attachProcess(targetPid: Int): Boolean =
        operationRunner.run {
            _state.update { current -> current.copy(targetPidInput = targetPid.toString()) }
            attachTargetUnsafe(targetPid)
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

    private suspend fun attachTargetUnsafe(targetPid: Int) {
        ensureSessionReadyUnsafe()
        unsafeOps.attachTarget(targetPid)
        unsafeOps.refreshThreads()
        unsafeOps.refreshVmas()
        unsafeOps.refreshImages()
        unsafeOps.refreshEvents(timeoutMs = 0, maxEvents = 16)
    }
}
