package com.smlc666.lkmdbg.overlay.presentation.memory

import org.junit.Assert.assertEquals
import org.junit.Test

class MemoryWorkspaceReducerTest {
    @Test
    fun switchTab_doesNotCopySelectionBetweenTabs() {
        val base = MemoryWorkspaceState.initial()
        val initial = base.copy(
            activeTab = MemoryTab.Search,
            search = base.search.copy(selection = setOf(0x1000uL)),
        )

        val switched = MemoryWorkspaceReducer.reduce(
            initial,
            MemoryWorkspaceIntent.SwitchTab(MemoryTab.Saved),
        )

        assertEquals(MemoryTab.Saved, switched.activeTab)
        assertEquals(setOf(0x1000uL), switched.search.selection)
        assertEquals(emptySet(), switched.saved.selection)
        assertEquals(emptySet(), switched.page.selection)
    }
}
