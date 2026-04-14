package com.smlc666.lkmdbg.overlay.presentation.workspace

object WorkspaceReducer {
    fun reduce(state: WorkspaceUiState, action: WorkspaceAction): WorkspaceUiState = when (action) {
        is WorkspaceAction.SetSection -> state.copy(section = action.section)
    }
}
