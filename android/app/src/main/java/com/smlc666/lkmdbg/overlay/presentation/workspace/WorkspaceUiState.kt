package com.smlc666.lkmdbg.overlay.presentation.workspace

import com.smlc666.lkmdbg.data.WorkspaceSection
import com.smlc666.lkmdbg.domain.gateway.EventEntry
import com.smlc666.lkmdbg.domain.gateway.ProcessRecord
import com.smlc666.lkmdbg.domain.gateway.ThreadRecord
import com.smlc666.lkmdbg.shared.BridgeThreadRegistersReply

data class WorkspaceUiState(
    val section: WorkspaceSection,
    val session: SessionUiState,
    val processes: ProcessUiState = ProcessUiState(),
    val threads: ThreadUiState = ThreadUiState(),
    val events: EventUiState = EventUiState(),
) {
    companion object {
        fun initial(): WorkspaceUiState = WorkspaceUiState(
            section = WorkspaceSection.Memory,
            session = SessionUiState(),
        )
    }
}

data class SessionUiState(
    val isConnected: Boolean = false,
    val isSessionOpen: Boolean = false,
    val message: String = "",
)

data class ProcessUiState(
    val items: List<ProcessRecord> = emptyList(),
    val message: String = "",
)

data class ThreadUiState(
    val items: List<ThreadRecord> = emptyList(),
    val selectedTid: Int? = null,
    val selectedRegisters: BridgeThreadRegistersReply? = null,
    val message: String = "",
)

data class EventUiState(
    val items: List<EventEntry> = emptyList(),
    val pinnedEventSeqs: Set<ULong> = emptySet(),
    val message: String = "",
)
