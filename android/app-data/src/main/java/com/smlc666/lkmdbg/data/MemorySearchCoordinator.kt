package com.smlc666.lkmdbg.data

import android.content.Context
import com.smlc666.lkmdbg.appdata.R
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.update

internal class MemorySearchCoordinator(
    context: Context,
    private val client: PipeAgentClient,
    private val stateFlow: MutableStateFlow<SessionBridgeState>,
    private val searchEngine: MemorySearchEngine,
    private val snapshotController: MemorySearchSnapshotController,
) {
    private val appContext = context.applicationContext

    suspend fun runSearch(refreshVmas: suspend () -> Unit) {
        if (stateFlow.value.vmas.isEmpty())
            refreshVmas()

        val search = stateFlow.value.memorySearch
        if (search.refineMode != MemorySearchRefineMode.Exact) {
            captureSnapshot(search.regionPreset, search.valueType)
            return
        }

        discardSnapshot()
        val outcome = searchEngine.search(
            vmas = stateFlow.value.vmas,
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
        stateFlow.update { current ->
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

    suspend fun refineSearch(refreshVmas: suspend () -> Unit) {
        val currentResults = stateFlow.value.memorySearch.results
        if (stateFlow.value.vmas.isEmpty())
            refreshVmas()

        val search = stateFlow.value.memorySearch
        if (currentResults.isEmpty()) {
            if (search.refineMode != MemorySearchRefineMode.Exact && search.snapshotReady) {
                val outcome = refineSnapshot(
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
                stateFlow.update { current ->
                    current.copy(
                        memorySearch = current.memorySearch.copy(
                            summary = summary,
                            results = outcome.results,
                        ),
                        lastMessage = summary,
                    )
                }
                return
            }
            throw IllegalStateException(appContext.getString(R.string.memory_search_refine_empty))
        }

        val outcome = searchEngine.refine(
            vmas = stateFlow.value.vmas,
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
        stateFlow.update { current ->
            current.copy(
                memorySearch = current.memorySearch.copy(
                    summary = summary,
                    results = outcome.results,
                ),
                lastMessage = summary,
            )
        }
    }

    fun discardSnapshot() {
        snapshotController.discard()
        stateFlow.update { current ->
            current.copy(
                memorySearch = current.memorySearch.copy(
                    snapshotReady = false,
                ),
            )
        }
    }

    private suspend fun captureSnapshot(
        regionPreset: MemoryRegionPreset,
        valueType: MemorySearchValueType,
    ) {
        if (!snapshotController.supportsUnknownInitial(valueType)) {
            throw IllegalStateException(appContext.getString(R.string.memory_search_fuzzy_requires_numeric))
        }

        val matchingVmas = stateFlow.value.vmas.filter(regionPreset::matches)
        if (matchingVmas.isEmpty()) {
            throw IllegalStateException(appContext.getString(R.string.memory_ranges_empty))
        }

        snapshotController.discard()
        val captured = snapshotController.capture(
            targetPid = stateFlow.value.snapshot.targetPid,
            regionPreset = regionPreset,
            matchingVmas = matchingVmas,
            readMemory = client::readMemory,
        )
        val summary = appContext.getString(
            R.string.memory_search_snapshot_captured,
            captured.rangeCount,
            captured.totalBytes.toString(),
        )
        stateFlow.update { current ->
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

    private suspend fun refineSnapshot(
        regionPreset: MemoryRegionPreset,
        valueType: MemorySearchValueType,
        refineMode: MemorySearchRefineMode,
    ): MemorySearchOutcome {
        if (!snapshotController.supportsUnknownInitial(valueType)) {
            throw IllegalStateException(appContext.getString(R.string.memory_search_fuzzy_requires_numeric))
        }
        return try {
            snapshotController.refine(
                targetPid = stateFlow.value.snapshot.targetPid,
                regionPreset = regionPreset,
                valueType = valueType,
                refineMode = refineMode,
                vmas = stateFlow.value.vmas,
                readMemory = client::readMemory,
            )
        } catch (e: IllegalStateException) {
            if (e.message == "missing snapshot") {
                snapshotController.discard()
                throw IllegalStateException(appContext.getString(R.string.memory_search_snapshot_missing))
            }
            throw e
        }
    }
}
