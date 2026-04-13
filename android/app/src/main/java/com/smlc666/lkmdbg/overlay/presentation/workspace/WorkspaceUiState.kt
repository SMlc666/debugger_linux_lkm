package com.smlc666.lkmdbg.overlay.presentation.workspace

import com.smlc666.lkmdbg.data.WorkspaceSection

data class WorkspaceUiState(
    val section: WorkspaceSection,
    val session: SessionUiState,
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
)
