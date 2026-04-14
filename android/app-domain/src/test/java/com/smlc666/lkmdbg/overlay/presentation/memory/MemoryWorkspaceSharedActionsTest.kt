package com.smlc666.lkmdbg.overlay.presentation.memory

import org.junit.Assert.assertEquals
import org.junit.Test

class MemoryWorkspaceSharedActionsTest {
    @Test
    fun filterText_isIndependentPerTab() {
        val initial = MemoryWorkspaceState.initial()
        val a = MemoryWorkspaceReducer.reduce(
            initial,
            MemoryWorkspaceIntent.SetFilter(MemoryTab.Search, "hp"),
        )
        val b = MemoryWorkspaceReducer.reduce(
            a,
            MemoryWorkspaceIntent.SetFilter(MemoryTab.Saved, "ammo"),
        )

        assertEquals("hp", b.search.filterText)
        assertEquals("ammo", b.saved.filterText)
        assertEquals("", b.page.filterText)
    }

    @Test
    fun toggleSelected_isIndependentPerTab() {
        val initial = MemoryWorkspaceState.initial()
        val a = MemoryWorkspaceReducer.reduce(
            initial,
            MemoryWorkspaceIntent.ToggleSelected(MemoryTab.Page, 0x1234uL),
        )
        val b = MemoryWorkspaceReducer.reduce(
            a,
            MemoryWorkspaceIntent.ToggleSelected(MemoryTab.Saved, 0x1234uL),
        )

        assertEquals(setOf(0x1234uL), b.page.selection)
        assertEquals(setOf(0x1234uL), b.saved.selection)
        assertEquals(emptySet(), b.search.selection)
    }
}
