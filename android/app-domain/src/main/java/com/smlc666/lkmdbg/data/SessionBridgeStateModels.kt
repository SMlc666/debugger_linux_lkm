package com.smlc666.lkmdbg.data

import com.smlc666.lkmdbg.shared.BridgeEventRecord
import com.smlc666.lkmdbg.shared.BridgeHelloReply
import com.smlc666.lkmdbg.shared.BridgeHwpointRecord
import com.smlc666.lkmdbg.shared.BridgeImageRecord
import com.smlc666.lkmdbg.shared.BridgeStatusSnapshot
import com.smlc666.lkmdbg.shared.BridgeStopState
import com.smlc666.lkmdbg.shared.BridgeThreadRecord
import com.smlc666.lkmdbg.shared.BridgeThreadRegistersReply
import com.smlc666.lkmdbg.shared.BridgeVmaRecord

private const val HWPOINT_TYPE_WRITE = 0x00000002u
private const val HWPOINT_TYPE_EXEC = 0x00000004u
private const val HWPOINT_FLAG_MMU = 0x00000002u

data class SessionEventEntry(
    val record: BridgeEventRecord,
    val receivedAtMs: Long,
)

enum class HwpointPreset(
    val type: UInt,
    val flags: UInt,
    val defaultLength: Int,
) {
    ExecHardware(
        type = HWPOINT_TYPE_EXEC,
        flags = 0u,
        defaultLength = 4,
    ),
    ExecMmu(
        type = HWPOINT_TYPE_EXEC,
        flags = HWPOINT_FLAG_MMU,
        defaultLength = 4,
    ),
    WriteHardware(
        type = HWPOINT_TYPE_WRITE,
        flags = 0u,
        defaultLength = 4,
    ),
    WriteMmu(
        type = HWPOINT_TYPE_WRITE,
        flags = HWPOINT_FLAG_MMU,
        defaultLength = 4,
    ),
}

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
    val sessionControlsBusy: Boolean = false,
    val agentPath: String,
    val workspaceSection: WorkspaceSection = WorkspaceSection.Memory,
    val targetPidInput: String = "",
    val targetTidInput: String = "",
    val selectedProcessPid: Int? = null,
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
    val stopState: BridgeStopState? = null,
    val lastMessage: String,
    val processFilter: ProcessFilter = ProcessFilter.All,
    val processes: List<ResolvedProcessRecord> = emptyList(),
    val threads: List<BridgeThreadRecord> = emptyList(),
    val selectedThreadTid: Int? = null,
    val selectedThreadRegisters: BridgeThreadRegistersReply? = null,
    val recentEvents: List<SessionEventEntry> = emptyList(),
    val pinnedEventSeqs: Set<ULong> = emptySet(),
    val eventsAutoPollEnabled: Boolean = false,
    val images: List<BridgeImageRecord> = emptyList(),
    val vmas: List<BridgeVmaRecord> = emptyList(),
    val hwpoints: List<BridgeHwpointRecord> = emptyList(),
    val selectedHwpointId: ULong? = null,
    val hwpointAddressInput: String = "",
    val hwpointLengthInput: String = HwpointPreset.ExecHardware.defaultLength.toString(),
    val hwpointPreset: HwpointPreset = HwpointPreset.ExecHardware,
    val memoryAddressInput: String = "",
    val memorySelectionSize: Int = 4,
    val memoryWriteHexInput: String = "",
    val memoryWriteAsciiInput: String = "",
    val memoryWriteAsmInput: String = "",
    val memoryPage: MemoryPage? = null,
    val memorySearch: MemorySearchUiState = MemorySearchUiState(),
)
