package com.smlc666.lkmdbg.data

import android.content.Context
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.shared.BridgeEventBatchReply
import com.smlc666.lkmdbg.shared.BridgeEventRecord
import com.smlc666.lkmdbg.shared.BridgeHelloReply
import com.smlc666.lkmdbg.shared.BridgeImageListReply
import com.smlc666.lkmdbg.shared.BridgeImageRecord
import com.smlc666.lkmdbg.shared.BridgeOpenSessionReply
import com.smlc666.lkmdbg.shared.BridgeProcessListReply
import com.smlc666.lkmdbg.shared.BridgeStatusSnapshot
import com.smlc666.lkmdbg.shared.BridgeThreadListReply
import com.smlc666.lkmdbg.shared.BridgeThreadRecord
import com.smlc666.lkmdbg.shared.BridgeThreadRegistersReply
import com.smlc666.lkmdbg.shared.BridgeVmaListReply
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
        runOperation { connectUnsafe() }
    }

    suspend fun openSession() {
        runOperation {
            if (state.value.hello == null)
                connectUnsafe()
            openSessionUnsafe()
            refreshStatusUnsafe()
            if (state.value.processes.isEmpty())
                refreshProcessesUnsafe()
        }
    }

    suspend fun refreshStatus() {
        runOperation { refreshStatusUnsafe() }
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
            attachTargetUnsafe(targetPid)
            refreshThreadsUnsafe()
            refreshVmasUnsafe()
            refreshImagesUnsafe()
            refreshEventsUnsafe(timeoutMs = 0, maxEvents = 16)
        }
    }

    suspend fun refreshProcesses() {
        runOperation {
            refreshProcessesUnsafe()
        }
    }

    suspend fun refreshThreads() {
        runOperation {
            if (!hasActiveTarget()) {
                clearThreadState()
                updateMessage(appContext.getString(R.string.thread_error_no_target))
                return@runOperation
            }
            refreshThreadsUnsafe()
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
            refreshEventsUnsafe(timeoutMs, maxEvents)
        }
    }

    suspend fun refreshImages() {
        runOperation {
            if (!hasActiveTarget()) {
                _state.update { current -> current.copy(images = emptyList()) }
                updateMessage(appContext.getString(R.string.memory_error_no_target))
                return@runOperation
            }
            refreshImagesUnsafe()
        }
    }

    suspend fun refreshVmas() {
        runOperation {
            if (!hasActiveTarget()) {
                _state.update { current -> current.copy(vmas = emptyList()) }
                updateMessage(appContext.getString(R.string.memory_error_no_target))
                return@runOperation
            }
            refreshVmasUnsafe()
        }
    }

    suspend fun runMemorySearch() {
        runOperation {
            if (!hasActiveTarget()) {
                updateMessage(appContext.getString(R.string.memory_error_no_target))
                return@runOperation
            }
            if (state.value.vmas.isEmpty())
                refreshVmasUnsafe()

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
                refreshVmasUnsafe()

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
            openMemoryPageUnsafe(remoteAddr)
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
            openMemoryPageUnsafe(pc)
        }
    }

    suspend fun jumpToMemoryAddress() {
        val address = parseAddressInput(state.value.memoryAddressInput)
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
            openMemoryPageUnsafe(nextFocus)
        }
    }

    suspend fun loadSelectionIntoHexSearch() {
        runOperation {
            val bytes = currentSelectionBytes() ?: run {
                updateMessage(appContext.getString(R.string.memory_error_no_selection))
                return@runOperation
            }
            val query = bytes.joinToString(" ") { "%02x".format(it.toInt() and 0xff) }
            _state.update { current ->
                current.copy(
                    memorySearch = current.memorySearch.copy(
                        valueType = MemorySearchValueType.HexBytes,
                        query = query,
                    ),
                    lastMessage = appContext.getString(R.string.memory_message_search_query_loaded, query),
                )
            }
        }
    }

    suspend fun loadSelectionIntoAsciiSearch() {
        runOperation {
            val bytes = currentSelectionBytes() ?: run {
                updateMessage(appContext.getString(R.string.memory_error_no_selection))
                return@runOperation
            }
            val query = bytes.decodeToString()
            _state.update { current ->
                current.copy(
                    memorySearch = current.memorySearch.copy(
                        valueType = MemorySearchValueType.Ascii,
                        query = query,
                    ),
                    lastMessage = appContext.getString(R.string.memory_message_search_query_loaded, query),
                )
            }
        }
    }

    suspend fun loadSelectionIntoEditors() {
        runOperation {
            val bytes = currentSelectionBytes() ?: run {
                updateMessage(appContext.getString(R.string.memory_error_no_selection))
                return@runOperation
            }
            _state.update { current ->
                current.copy(
                    memoryWriteHexInput = bytes.joinToString(" ") { "%02x".format(it.toInt() and 0xff) },
                    memoryWriteAsciiInput = bytes.decodeToString(),
                    lastMessage = appContext.getString(R.string.memory_message_editor_loaded, bytes.size),
                )
            }
        }
    }

    suspend fun assembleArm64ToEditors() {
        runOperation {
            val page = state.value.memoryPage ?: run {
                updateMessage(appContext.getString(R.string.memory_error_no_page))
                return@runOperation
            }
            val source = state.value.memoryWriteAsmInput.trim()
            if (source.isEmpty()) {
                updateMessage(appContext.getString(R.string.memory_error_invalid_asm))
                return@runOperation
            }
            val encoded = NativeAssembler.assembleArm64(page.focusAddress, source)
            if (encoded.isEmpty()) {
                updateMessage(appContext.getString(R.string.memory_error_invalid_asm))
                return@runOperation
            }
            _state.update { current ->
                current.copy(
                    memoryWriteHexInput = encoded.joinToString(" ") { "%02x".format(it.toInt() and 0xff) },
                    memoryWriteAsciiInput = encoded.decodeToString(),
                    lastMessage = appContext.getString(
                        R.string.memory_message_asm_complete,
                        encoded.size,
                        hex64(page.focusAddress),
                    ),
                )
            }
        }
    }

    suspend fun assembleArm64AndWrite() {
        runOperation {
            val page = state.value.memoryPage ?: run {
                updateMessage(appContext.getString(R.string.memory_error_no_page))
                return@runOperation
            }
            val source = state.value.memoryWriteAsmInput.trim()
            if (source.isEmpty()) {
                updateMessage(appContext.getString(R.string.memory_error_invalid_asm))
                return@runOperation
            }
            val encoded = NativeAssembler.assembleArm64(page.focusAddress, source)
            if (encoded.isEmpty()) {
                updateMessage(appContext.getString(R.string.memory_error_invalid_asm))
                return@runOperation
            }
            val reply = client.writeMemory(page.focusAddress, encoded)
            if (reply.status != 0 || reply.bytesDone.toInt() != encoded.size)
                throw IllegalStateException(reply.message.ifBlank { "write failed bytes_done=${reply.bytesDone}" })
            openMemoryPageUnsafe(page.focusAddress)
            _state.update { current ->
                current.copy(
                    memoryWriteHexInput = encoded.joinToString(" ") { "%02x".format(it.toInt() and 0xff) },
                    memoryWriteAsciiInput = encoded.decodeToString(),
                    lastMessage = appContext.getString(
                        R.string.memory_message_asm_write_complete,
                        encoded.size,
                        hex64(page.focusAddress),
                    ),
                )
            }
        }
    }

    suspend fun writeHexAtFocus() {
        runOperation {
            val page = state.value.memoryPage ?: run {
                updateMessage(appContext.getString(R.string.memory_error_no_page))
                return@runOperation
            }
            val data = parseHexBytes(state.value.memoryWriteHexInput)
            if (data == null || data.isEmpty()) {
                updateMessage(appContext.getString(R.string.memory_error_invalid_hex))
                return@runOperation
            }
            val reply = client.writeMemory(page.focusAddress, data)
            if (reply.status != 0 || reply.bytesDone.toInt() != data.size)
                throw IllegalStateException(reply.message.ifBlank { "write failed bytes_done=${reply.bytesDone}" })
            openMemoryPageUnsafe(page.focusAddress)
            _state.update { current ->
                current.copy(
                    memoryWriteHexInput = data.joinToString(" ") { "%02x".format(it.toInt() and 0xff) },
                    memoryWriteAsciiInput = data.decodeToString(),
                    lastMessage = reply.message.ifBlank {
                        appContext.getString(
                            R.string.memory_message_write_complete,
                            data.size,
                            hex64(page.focusAddress),
                        )
                    },
                )
            }
        }
    }

    suspend fun writeAsciiAtFocus() {
        runOperation {
            val page = state.value.memoryPage ?: run {
                updateMessage(appContext.getString(R.string.memory_error_no_page))
                return@runOperation
            }
            val data = state.value.memoryWriteAsciiInput.toByteArray(Charsets.UTF_8)
            if (data.isEmpty()) {
                updateMessage(appContext.getString(R.string.memory_error_invalid_ascii))
                return@runOperation
            }
            val reply = client.writeMemory(page.focusAddress, data)
            if (reply.status != 0 || reply.bytesDone.toInt() != data.size)
                throw IllegalStateException(reply.message.ifBlank { "write failed bytes_done=${reply.bytesDone}" })
            openMemoryPageUnsafe(page.focusAddress)
            _state.update { current ->
                current.copy(
                    memoryWriteHexInput = data.joinToString(" ") { "%02x".format(it.toInt() and 0xff) },
                    memoryWriteAsciiInput = data.decodeToString(),
                    lastMessage = reply.message.ifBlank {
                        appContext.getString(
                            R.string.memory_message_write_complete,
                            data.size,
                            hex64(page.focusAddress),
                        )
                    },
                )
            }
        }
    }

    suspend fun selectMemoryAddress(remoteAddr: ULong) {
        runOperation {
            val current = state.value.memoryPage
            if (current != null && memoryPreviewBuilder.addressInsidePage(current, remoteAddr)) {
                _state.update { snapshot ->
                    val page = snapshot.memoryPage ?: return@update snapshot
                    snapshot.copy(
                        memoryAddressInput = hex64(remoteAddr),
                        memoryPage = memoryPreviewBuilder.retargetPage(page, remoteAddr),
                        lastMessage = appContext.getString(R.string.memory_message_cursor, hex64(remoteAddr)),
                    )
                }
                return@runOperation
            }
            openMemoryPageUnsafe(remoteAddr)
        }
    }

    suspend fun attachProcess(targetPid: Int): Boolean =
        runOperation {
            _state.update { current -> current.copy(targetPidInput = targetPid.toString()) }
            ensureSessionReadyUnsafe()
            attachTargetUnsafe(targetPid)
            refreshThreadsUnsafe()
            refreshVmasUnsafe()
            refreshImagesUnsafe()
            refreshEventsUnsafe(timeoutMs = 0, maxEvents = 16)
            true
        } ?: false

    private fun hasActiveTarget(): Boolean = state.value.snapshot.targetPid > 0

    private suspend fun ensureSessionReadyUnsafe() {
        if (state.value.hello == null)
            connectUnsafe()
        if (!state.value.snapshot.sessionOpen)
            openSessionUnsafe()
        refreshStatusUnsafe()
    }

    private suspend fun connectUnsafe(): BridgeHelloReply {
        val reply = client.connect()
        ensureBridgeStatusOk(reply.status, reply.message, "HELLO")
        _state.update { current ->
            current.copy(
                hello = reply,
                lastMessage = reply.message.ifBlank {
                    appContext.getString(R.string.session_message_connected)
                },
            )
        }
        return reply
    }

    private suspend fun openSessionUnsafe(): BridgeOpenSessionReply {
        val reply = client.openSession()
        ensureBridgeStatusOk(reply.status, reply.message, "OPEN_SESSION")
        if (!reply.sessionOpen)
            throw IllegalStateException(reply.message.ifBlank { "OPEN_SESSION did not open a session" })
        _state.update { current ->
            current.copy(
                lastMessage = reply.message.ifBlank {
                    appContext.getString(R.string.session_message_opened)
                },
            )
        }
        return reply
    }

    private suspend fun refreshStatusUnsafe(): BridgeStatusSnapshot {
        val snapshot = client.statusSnapshot()
        ensureBridgeStatusOk(snapshot.status, snapshot.message, "STATUS_SNAPSHOT")
        _state.update { current ->
            current.copy(
                snapshot = snapshot,
                lastMessage = snapshot.message.ifBlank {
                    appContext.getString(R.string.session_message_refreshed)
                },
            )
        }
        return snapshot
    }

    private suspend fun attachTargetUnsafe(targetPid: Int) {
        discardMemorySearchSnapshot()
        val reply = client.setTarget(targetPid)
        ensureBridgeStatusOk(reply.status, reply.message, "SET_TARGET")
        _state.update { current ->
            current.copy(
                memorySearch = current.memorySearch.copy(
                    snapshotReady = false,
                    summary = "",
                    results = emptyList(),
                ),
                lastMessage = reply.message.ifBlank {
                    appContext.getString(R.string.session_message_target_attached, targetPid)
                },
            )
        }
        refreshStatusUnsafe()
    }

    private suspend fun refreshThreadsUnsafe(preferredTid: Int? = null) {
        val reply: BridgeThreadListReply = client.queryThreads()
        ensureBridgeStatusOk(reply.status, reply.message, "QUERY_THREADS")
        val selectedTid = selectThreadId(reply.threads, preferredTid)
        val registers = selectedTid?.let {
            runCatching {
                client.getRegisters(it).also { registerReply ->
                    ensureBridgeStatusOk(registerReply.status, registerReply.message, "GET_REGISTERS")
                }
            }.getOrNull()
        }
        _state.update { current ->
            current.copy(
                threads = reply.threads,
                selectedThreadTid = selectedTid,
                selectedThreadRegisters = registers,
                lastMessage = reply.message.ifBlank {
                    appContext.getString(R.string.thread_message_refreshed, reply.threads.size)
                },
            )
        }
    }

    private suspend fun refreshEventsUnsafe(timeoutMs: Int, maxEvents: Int) {
        val reply: BridgeEventBatchReply = client.pollEvents(timeoutMs, maxEvents)
        ensureBridgeStatusOk(reply.status, reply.message, "POLL_EVENT")
        val now = System.currentTimeMillis()
        _state.update { current ->
            val merged = buildList {
                reply.events.forEach { add(SessionEventEntry(it, now)) }
                current.recentEvents.forEach { existing ->
                    if (none { it.record.seq == existing.record.seq })
                        add(existing)
                }
            }.take(64)
            current.copy(
                recentEvents = merged,
                lastMessage = reply.message.ifBlank {
                    appContext.getString(R.string.event_message_refreshed, reply.events.size)
                },
            )
        }
    }

    private suspend fun refreshImagesUnsafe() {
        val reply: BridgeImageListReply = client.queryImages()
        ensureBridgeStatusOk(reply.status, reply.message, "QUERY_IMAGES")
        _state.update { current ->
            current.copy(
                images = reply.images,
                lastMessage = reply.message.ifBlank {
                    appContext.getString(R.string.memory_message_images, reply.images.size)
                },
            )
        }
    }

    private suspend fun refreshVmasUnsafe() {
        val reply: BridgeVmaListReply = client.queryVmas()
        ensureBridgeStatusOk(reply.status, reply.message, "QUERY_VMAS")
        _state.update { current ->
            current.copy(
                vmas = reply.vmas,
                memoryPage = current.memoryPage?.let { page ->
                    memoryPreviewBuilder.buildPage(page.focusAddress, page.pageStart, page.bytes, reply.vmas)
                },
                lastMessage = reply.message.ifBlank {
                    appContext.getString(R.string.memory_message_vmas, reply.vmas.size)
                },
            )
        }
    }

    private suspend fun openMemoryPageUnsafe(remoteAddr: ULong) {
        val pageStart = memoryPreviewBuilder.alignDown(remoteAddr)
        val reply = client.readMemory(pageStart, MEMORY_BROWSER_PAGE_SIZE)
        ensureBridgeStatusOk(reply.status, reply.message, "READ_MEMORY")
        val bytes = reply.data.copyOf(reply.bytesDone.toInt())
        _state.update { current ->
            current.copy(
                memoryAddressInput = hex64(remoteAddr),
                memoryPage = memoryPreviewBuilder.buildPage(remoteAddr, pageStart, bytes, current.vmas),
                lastMessage = reply.message.ifBlank {
                    appContext.getString(
                        R.string.memory_message_preview,
                        reply.bytesDone.toString(),
                        hex64(pageStart),
                    )
                },
            )
        }
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
        )
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

    private fun currentSelectionBytes(): ByteArray? {
        val page = state.value.memoryPage ?: return null
        return memoryPreviewBuilder.selectionBytes(page, state.value.memorySelectionSize)
    }

    private fun parseAddressInput(value: String): ULong? {
        val trimmed = value.trim()
        if (trimmed.isEmpty())
            return null
        if (trimmed.startsWith("0x", ignoreCase = true))
            return trimmed.removePrefix("0x").removePrefix("0X").toULongOrNull(16)
        return trimmed.toULongOrNull() ?: trimmed.toULongOrNull(16)
    }

    private fun parseHexBytes(value: String): ByteArray? {
        val compact = value.replace(" ", "").replace("\n", "").replace("\t", "")
        if (compact.isEmpty() || compact.length % 2 != 0)
            return null
        return runCatching {
            ByteArray(compact.length / 2) { index ->
                compact.substring(index * 2, index * 2 + 2).toInt(16).toByte()
            }
        }.getOrNull()
    }

    private fun selectThreadId(
        threads: List<BridgeThreadRecord>,
        preferredTid: Int?,
    ): Int? {
        if (threads.isEmpty())
            return null
        val current = preferredTid
            ?: state.value.selectedThreadTid
            ?: state.value.snapshot.targetTid.takeIf { it > 0 }
        if (current != null && threads.any { it.tid == current })
            return current
        return threads.firstOrNull()?.tid
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

    private suspend fun refreshProcessesUnsafe() {
        val reply: BridgeProcessListReply = client.queryProcesses()
        ensureBridgeStatusOk(reply.status, reply.message, "QUERY_PROCESSES")
        val resolved = processResolver.resolve(reply.processes)
        _state.update { current ->
            current.copy(
                processes = resolved,
                lastMessage = reply.message.ifBlank {
                    appContext.getString(R.string.process_message_refreshed, resolved.size)
                },
            )
        }
    }

    private fun ensureBridgeStatusOk(status: Int, message: String, command: String) {
        if (status < 0)
            throw IllegalStateException(message.ifBlank { "$command failed with status=$status" })
    }

    private fun hex64(value: ULong): String = "0x${value.toString(16)}"

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
