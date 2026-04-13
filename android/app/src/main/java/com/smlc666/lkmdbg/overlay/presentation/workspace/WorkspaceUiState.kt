package com.smlc666.lkmdbg.overlay.presentation.workspace

import com.smlc666.lkmdbg.data.WorkspaceSection
import com.smlc666.lkmdbg.domain.gateway.ProcessRecord

data class WorkspaceUiState(
    val section: WorkspaceSection,
    val session: SessionUiState,
    val processes: ProcessUiState = ProcessUiState(),
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
