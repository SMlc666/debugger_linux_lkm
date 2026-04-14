package com.smlc666.lkmdbg.overlay.presentation.workspace

import com.smlc666.lkmdbg.data.WorkspaceSection

sealed interface WorkspaceAction {
    data class SetSection(val section: WorkspaceSection) : WorkspaceAction
}
