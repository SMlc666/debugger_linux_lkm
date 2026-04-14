package com.smlc666.lkmdbg.platform.overlay

import com.smlc666.lkmdbg.overlay.presentation.workspace.WorkspaceUiState

data class OverlayHostUiModel(
    val showWorkspace: Boolean,
    val showCollapsedChip: Boolean,
)

class OverlayHostUiBinder {
    fun bind(state: WorkspaceUiState, expanded: Boolean): OverlayHostUiModel {
        // State is intentionally unused for now. The binder exists to centralize future
        // host-only view composition decisions without growing the Service.
        return OverlayHostUiModel(
            showWorkspace = expanded,
            showCollapsedChip = !expanded,
        )
    }
}

