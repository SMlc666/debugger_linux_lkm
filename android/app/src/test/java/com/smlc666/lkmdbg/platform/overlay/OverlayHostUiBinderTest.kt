package com.smlc666.lkmdbg.platform.overlay

import com.smlc666.lkmdbg.overlay.presentation.workspace.WorkspaceUiState
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class OverlayHostUiBinderTest {
    @Test
    fun bindState_forExpandedMode_buildsWorkspaceUiModel() {
        val binder = OverlayHostUiBinder()
        val model = binder.bind(WorkspaceUiState.initial(), expanded = true)
        assertTrue(model.showWorkspace)
        assertFalse(model.showCollapsedChip)
    }

    @Test
    fun bindState_forCollapsedMode_buildsCollapsedChipModel() {
        val binder = OverlayHostUiBinder()
        val model = binder.bind(WorkspaceUiState.initial(), expanded = false)
        assertFalse(model.showWorkspace)
        assertTrue(model.showCollapsedChip)
    }
}
