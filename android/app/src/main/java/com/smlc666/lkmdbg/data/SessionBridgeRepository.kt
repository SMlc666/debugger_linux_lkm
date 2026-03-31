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
import java.io.IOException

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
    private val memoryEditor = MemoryEditorController(
        context = appContext,
        client = client,
        stateFlow = _state,
        memoryPreviewBuilder = memoryPreviewBuilder,
        pageSize = MEMORY_BROWSER_PAGE_SIZE,
    )
    private val memorySearchSnapshotController = MemorySearchSnapshotController(
        cacheDir = appContext.cacheDir,
        snapshotChunkSize = MEMORY_SEARCH_SNAPSHOT_CHUNK_SIZE,
    )
    private val _state = MutableStateFlow(
        SessionBridgeState(
            agentPath = client.agentPathHint,
            lastMessage = appContext.getString(R.string.session_message_idle),
        ),
    )
    private val unsafeOps = SessionBridgeUnsafeOps(
        context = appContext,
        client = client,
        stateFlow = _state,
        processResolver = processResolver,
        memoryPreviewBuilder = memoryPreviewBuilder,
        discardMemorySearchSnapshot = ::discardMemorySearchSnapshot,
    )
    val state: StateFlow<SessionBridgeState> = _state.asStateFlow()

    fun rootBridgeDiagnostics(): RootBridgeDiagnostics = client.diagnostics()

    fun updateTargetPidInput(value: String) {
        _state.update { current ->
            current.copy(targetPidInput = value.filter { it.isDigit() })
        }
    }

    fun updateProcessFilter(filter: ProcessFilter) {
        _state.update { current -> current.copy(processFilter = filter) }
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

    fun updateMemoryRegionPreset(regionPreset: MemoryRegionPreset) {
        discardMemorySearchSnapshot()
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

    suspend fun connect() {
        runOperation { unsafeOps.connect() }
    }

    suspend fun openSession() {
        runOperation {
            if (state.value.hello == null)
                unsafeOps.connect()
            unsafeOps.openSession()
            unsafeOps.refreshStatus()
            if (state.value.processes.isEmpty())
                unsafeOps.refreshProcesses()
        }
    }

    suspend fun refreshStatus() {
        runOperation { unsafeOps.refreshStatus() }
    }

    suspend fun attachTarget() {
        val targetPid = state.value.targetPidInput.toIntOrNull()
        if (targetPid == null || targetPid <= 0) {
            _state.update { current ->
                current.copy(lastMessage = appContext.getString(R.string.session_error_invalid_pid))
            }
            return
        }

        runOperation {
            ensureSessionReadyUnsafe()
            unsafeOps.attachTarget(targetPid)
            unsafeOps.refreshThreads()
            unsafeOps.refreshVmas()
            unsafeOps.refreshImages()
            unsafeOps.refreshEvents(timeoutMs = 0, maxEvents = 16)
        }
    }

    suspend fun refreshProcesses() {
        runOperation {
            unsafeOps.refreshProcesses()
        }
    }

    suspend fun refreshThreads() {
        runOperation {
            if (!hasActiveTarget()) {
                clearThreadState()
                updateMessage(appContext.getString(R.string.thread_error_no_target))
                return@runOperation
            }
            unsafeOps.refreshThreads()
        }
    }

    suspend fun refreshThreadRegisters(tid: Int) {
        runOperation {
            val reply = client.getRegisters(tid)
            _state.update { current ->
                current.copy(
                    selectedThreadTid = tid,
                    selectedThreadRegisters = reply,
                    lastMessage = reply.message.ifBlank {
                        appContext.getString(R.string.thread_message_registers, tid)
                    },
                )
            }
        }
    }

    suspend fun refreshEvents(timeoutMs: Int = 0, maxEvents: Int = 16) {
        runOperation {
            if (!state.value.snapshot.sessionOpen) {
                updateMessage(appContext.getString(R.string.event_error_no_session))
                return@runOperation
            }
            unsafeOps.refreshEvents(timeoutMs, maxEvents)
        }
    }

    suspend fun refreshImages() {
        runOperation {
            if (!hasActiveTarget()) {
                _state.update { current -> current.copy(images = emptyList()) }
                updateMessage(appContext.getString(R.string.memory_error_no_target))
                return@runOperation
            }
            unsafeOps.refreshImages()
        }
    }

    suspend fun refreshVmas() {
        runOperation {
            if (!hasActiveTarget()) {
                _state.update { current -> current.copy(vmas = emptyList()) }
                updateMessage(appContext.getString(R.string.memory_error_no_target))
                return@runOperation
            }
            unsafeOps.refreshVmas()
        }
    }

    suspend fun runMemorySearch() {
        runOperation {
            if (!hasActiveTarget()) {
                updateMessage(appContext.getString(R.string.memory_error_no_target))
                return@runOperation
            }
            if (state.value.vmas.isEmpty())
                unsafeOps.refreshVmas()

            val search = state.value.memorySearch
            if (search.refineMode != MemorySearchRefineMode.Exact) {
                captureMemorySearchSnapshotUnsafe(search.regionPreset, search.valueType)
                return@runOperation
            }

            discardMemorySearchSnapshot()
            val outcome = searchEngine.search(
                vmas = state.value.vmas,
                preset = search.regionPreset,
                valueType = search.valueType,
                query = search.query,
            )
            val summary = appContext.getString(
                R.string.memory_search_summary,
                outcome.results.size,
                outcome.searchedVmaCount,
                outcome.scannedBytes.toString(),
            )
            _state.update { current ->
                current.copy(
                    memorySearch = current.memorySearch.copy(
                        snapshotReady = false,
                        summary = summary,
                        results = outcome.results,
                    ),
                    lastMessage = summary,
                )
            }
        }
    }

    suspend fun refineMemorySearch() {
        runOperation {
            if (!hasActiveTarget()) {
                updateMessage(appContext.getString(R.string.memory_error_no_target))
                return@runOperation
            }
            val currentResults = state.value.memorySearch.results
            if (state.value.vmas.isEmpty())
                unsafeOps.refreshVmas()

            val search = state.value.memorySearch
            if (currentResults.isEmpty()) {
                if (search.refineMode != MemorySearchRefineMode.Exact &&
                    search.snapshotReady
                ) {
                    val outcome = refineMemorySearchSnapshotUnsafe(
                        search.regionPreset,
                        search.valueType,
                        search.refineMode,
                    )
                    val summary = appContext.getString(
                        R.string.memory_search_fuzzy_summary,
                        outcome.results.size,
                        outcome.searchedVmaCount,
                        outcome.scannedBytes.toString(),
                    )
                    _state.update { current ->
                        current.copy(
                            memorySearch = current.memorySearch.copy(
                                summary = summary,
                                results = outcome.results,
                            ),
                            lastMessage = summary,
                        )
                    }
                    return@runOperation
                }
                updateMessage(appContext.getString(R.string.memory_search_refine_empty))
                return@runOperation
            }

            val outcome = searchEngine.refine(
                vmas = state.value.vmas,
                sourceResults = currentResults,
                valueType = search.valueType,
                refineMode = search.refineMode,
                query = search.query,
                reader = { address, length ->
                    val reply = client.readMemory(address, length)
                    if (reply.status != 0) {
                        ByteArray(0)
                    } else {
                        reply.data.copyOf(reply.bytesDone.toInt())
                    }
                },
            )
            val summary = appContext.getString(
                R.string.memory_search_refine_summary,
                outcome.results.size,
                currentResults.size,
                outcome.scannedBytes.toString(),
            )
            _state.update { current ->
                current.copy(
                    memorySearch = current.memorySearch.copy(
                        summary = summary,
                        results = outcome.results,
                    ),
                    lastMessage = summary,
                )
            }
        }
    }

    suspend fun openMemoryPage(remoteAddr: ULong) {
        runOperation {
            memoryEditor.openPage(remoteAddr)
        }
    }

    suspend fun previewSelectedPc() {
        runOperation {
            val pc = state.value.selectedThreadRegisters?.pc
                ?: state.value.threads.firstOrNull()?.userPc
            if (pc == null || pc == 0uL) {
                updateMessage(appContext.getString(R.string.memory_error_no_pc))
                return@runOperation
            }
            memoryEditor.openPage(pc)
        }
    }

    suspend fun jumpToMemoryAddress() {
        val address = memoryEditor.parseAddressInput(state.value.memoryAddressInput)
        if (address == null) {
            updateMessage(appContext.getString(R.string.memory_error_invalid_address))
            return
        }
        openMemoryPage(address)
    }

    suspend fun stepMemoryPage(direction: Int) {
        runOperation {
            val current = state.value.memoryPage
            if (current == null) {
                updateMessage(appContext.getString(R.string.memory_error_no_page))
                return@runOperation
            }
            if (direction == 0)
                return@runOperation

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
        runOperation {
            memoryEditor.loadSelectionIntoHexSearch()
        }
    }

    suspend fun loadSelectionIntoAsciiSearch() {
        runOperation {
            memoryEditor.loadSelectionIntoAsciiSearch()
        }
    }

    suspend fun loadSelectionIntoEditors() {
        runOperation {
            memoryEditor.loadSelectionIntoEditors()
        }
    }

    suspend fun assembleArm64ToEditors() {
        runOperation {
            memoryEditor.assembleArm64ToEditors()
        }
    }

    suspend fun assembleArm64AndWrite() {
        runOperation {
            memoryEditor.assembleArm64AndWrite()
        }
    }

    suspend fun writeHexAtFocus() {
        runOperation {
            memoryEditor.writeHexAtFocus()
        }
    }

    suspend fun writeAsciiAtFocus() {
        runOperation {
            memoryEditor.writeAsciiAtFocus()
        }
    }

    suspend fun selectMemoryAddress(remoteAddr: ULong) {
        runOperation {
            memoryEditor.selectAddress(remoteAddr)
        }
    }

    suspend fun attachProcess(targetPid: Int): Boolean =
        runOperation {
            _state.update { current -> current.copy(targetPidInput = targetPid.toString()) }
            ensureSessionReadyUnsafe()
            unsafeOps.attachTarget(targetPid)
            unsafeOps.refreshThreads()
            unsafeOps.refreshVmas()
            unsafeOps.refreshImages()
            unsafeOps.refreshEvents(timeoutMs = 0, maxEvents = 16)
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

    private suspend fun captureMemorySearchSnapshotUnsafe(
        regionPreset: MemoryRegionPreset,
        valueType: MemorySearchValueType,
    ) {
        if (!memorySearchSnapshotController.supportsUnknownInitial(valueType)) {
            updateMessage(appContext.getString(R.string.memory_search_fuzzy_requires_numeric))
            return
        }

        val matchingVmas = state.value.vmas.filter(regionPreset::matches)
        if (matchingVmas.isEmpty()) {
            updateMessage(appContext.getString(R.string.memory_ranges_empty))
            return
        }

        memorySearchSnapshotController.discard()
        val captured = memorySearchSnapshotController.capture(
            targetPid = state.value.snapshot.targetPid,
            regionPreset = regionPreset,
            matchingVmas = matchingVmas,
            readMemory = client::readMemory,
        )
        val summary = appContext.getString(
            R.string.memory_search_snapshot_captured,
            captured.rangeCount,
            captured.totalBytes.toString(),
        )
        _state.update { current ->
            current.copy(
                memorySearch = current.memorySearch.copy(
                    snapshotReady = true,
                    summary = summary,
                    results = emptyList(),
                ),
                lastMessage = summary,
            )
        }
    }

    private suspend fun refineMemorySearchSnapshotUnsafe(
        regionPreset: MemoryRegionPreset,
        valueType: MemorySearchValueType,
        refineMode: MemorySearchRefineMode,
    ): MemorySearchOutcome {
        if (!memorySearchSnapshotController.supportsUnknownInitial(valueType)) {
            throw IllegalStateException(appContext.getString(R.string.memory_search_fuzzy_requires_numeric))
        }
        return try {
            memorySearchSnapshotController.refine(
                targetPid = state.value.snapshot.targetPid,
                regionPreset = regionPreset,
                valueType = valueType,
                refineMode = refineMode,
                vmas = state.value.vmas,
                readMemory = client::readMemory,
            )
        } catch (e: IllegalStateException) {
            if (e.message == "missing snapshot") {
                memorySearchSnapshotController.discard()
                throw IllegalStateException(appContext.getString(R.string.memory_search_snapshot_missing))
            }
            throw e
        }
    }

    private fun discardMemorySearchSnapshot() {
        memorySearchSnapshotController.discard()
        _state.update { current ->
            current.copy(
                memorySearch = current.memorySearch.copy(
                    snapshotReady = false,
                ),
            )
        }
    }

    private fun clearThreadState() {
        _state.update { current ->
            current.copy(
                threads = emptyList(),
                selectedThreadTid = null,
                selectedThreadRegisters = null,
            )
        }
    }

    private fun updateMessage(message: String) {
        _state.update { current -> current.copy(lastMessage = message) }
    }

    private suspend fun <T> runOperation(block: suspend () -> T): T? {
        _state.update { current -> current.copy(busy = true) }
        try {
            return block()
        } catch (e: IOException) {
            _state.update { current ->
                current.copy(
                    lastMessage = appContext.getString(
                        R.string.session_error_io,
                        e.message ?: appContext.getString(R.string.session_error_unknown),
                    ),
                )
            }
            return null
        } catch (e: IllegalStateException) {
            _state.update { current ->
                current.copy(
                    lastMessage = appContext.getString(
                        R.string.session_error_bridge,
                        e.message ?: appContext.getString(R.string.session_error_unknown),
                    ),
                )
            }
            return null
        } catch (e: RuntimeException) {
            _state.update { current ->
                current.copy(
                    lastMessage = appContext.getString(
                        R.string.session_error_bridge,
                        e.message ?: appContext.getString(R.string.session_error_unknown),
                    ),
                )
            }
            return null
        } finally {
            _state.update { current -> current.copy(busy = false) }
        }
    }
}
