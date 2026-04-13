package com.smlc666.lkmdbg.overlay.presentation.workspace

import com.smlc666.lkmdbg.data.WorkspaceSection

sealed interface WorkspaceIntent {
    data class SelectSection(val section: WorkspaceSection) : WorkspaceIntent
}
