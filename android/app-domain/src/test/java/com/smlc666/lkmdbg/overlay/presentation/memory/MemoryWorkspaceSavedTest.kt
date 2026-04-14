package com.smlc666.lkmdbg.overlay.presentation.memory

import org.junit.Assert.assertEquals
import org.junit.Test

class MemoryWorkspaceSavedTest {
    @Test
    fun addSelectionToSaved_onlyAffectsSavedEntries_notSelections() {
        val base = MemoryWorkspaceState.initial()
        val initial = base.copy(
            activeTab = MemoryTab.Search,
            search = base.search.copy(selection = setOf(0x1000uL, 0x2000uL)),
        )

        val next = MemoryWorkspaceReducer.reduce(
            initial,
            MemoryWorkspaceIntent.AddToSaved(addresses = initial.search.selection),
        )

        assertEquals(setOf(0x1000uL, 0x2000uL), next.saved.entries.keys.toSet())
        assertEquals(emptySet<ULong>(), next.saved.selection)
        assertEquals(setOf(0x1000uL, 0x2000uL), next.search.selection)
        assertEquals(emptySet<ULong>(), next.page.selection)
    }

    @Test
    fun removeFromSaved_onlyAffectsSavedSlice() {
        val base = MemoryWorkspaceState.initial()
        val savedEntries = LinkedHashMap<ULong, SavedMemoryEntry>().apply {
            put(0x1000uL, SavedMemoryEntry(address = 0x1000uL, label = "hp"))
            put(0x2000uL, SavedMemoryEntry(address = 0x2000uL, label = "ammo"))
        }
        val initial = base.copy(
            search = base.search.copy(selection = setOf(0x2000uL)),
            saved = base.saved.copy(entries = savedEntries, selection = setOf(0x1000uL, 0x2000uL)),
        )

        val next = MemoryWorkspaceReducer.reduce(
            initial,
            MemoryWorkspaceIntent.RemoveFromSaved(0x1000uL),
        )

        assertEquals(setOf(0x2000uL), next.saved.entries.keys.toSet())
        assertEquals(setOf(0x2000uL), next.saved.selection)
        assertEquals(setOf(0x2000uL), next.search.selection)
    }
}
