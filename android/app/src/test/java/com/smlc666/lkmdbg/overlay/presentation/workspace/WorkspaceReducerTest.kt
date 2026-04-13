package com.smlc666.lkmdbg.overlay.presentation.workspace

import com.smlc666.lkmdbg.data.WorkspaceSection
import org.junit.Assert.assertEquals
import org.junit.Test

class WorkspaceReducerTest {
    @Test
    fun reduce_setSection_updatesSectionAndLeavesOtherSlices() {
        val initial = WorkspaceUiState.initial()

        val next = WorkspaceReducer.reduce(
            initial,
            WorkspaceAction.SetSection(WorkspaceSection.Threads),
        )

        assertEquals(WorkspaceSection.Threads, next.section)
        assertEquals(initial.session, next.session)
    }
}
