package com.smlc666.lkmdbg.overlay.presentation.memory

data class SavedMemoryEntry(
    val address: ULong,
    val label: String = "",
)

data class MemorySearchViewState(
    val selection: Set<ULong> = emptySet(),
    val filterText: String = "",
)

data class MemorySavedViewState(
    val entries: LinkedHashMap<ULong, SavedMemoryEntry> = LinkedHashMap(),
    val selection: Set<ULong> = emptySet(),
    val filterText: String = "",
)

data class MemoryPageViewState(
    val selection: Set<ULong> = emptySet(),
    val filterText: String = "",
)

data class MemoryWorkspaceState(
    val activeTab: MemoryTab,
    val search: MemorySearchViewState,
    val saved: MemorySavedViewState,
    val page: MemoryPageViewState,
) {
    companion object {
        fun initial(): MemoryWorkspaceState =
            MemoryWorkspaceState(
                activeTab = MemoryTab.Search,
                search = MemorySearchViewState(),
                saved = MemorySavedViewState(),
                page = MemoryPageViewState(),
            )
    }
}

