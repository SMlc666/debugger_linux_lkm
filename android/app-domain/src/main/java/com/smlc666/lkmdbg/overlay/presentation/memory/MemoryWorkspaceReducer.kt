package com.smlc666.lkmdbg.overlay.presentation.memory

object MemoryWorkspaceReducer {
    fun reduce(state: MemoryWorkspaceState, intent: MemoryWorkspaceIntent): MemoryWorkspaceState =
        when (intent) {
            is MemoryWorkspaceIntent.SwitchTab -> state.copy(activeTab = intent.tab)

            is MemoryWorkspaceIntent.SetFilter -> when (intent.tab) {
                MemoryTab.Search -> state.copy(search = state.search.copy(filterText = intent.text))
                MemoryTab.Saved -> state.copy(saved = state.saved.copy(filterText = intent.text))
                MemoryTab.Page -> state.copy(page = state.page.copy(filterText = intent.text))
            }

            is MemoryWorkspaceIntent.ToggleSelected -> when (intent.tab) {
                MemoryTab.Search ->
                    state.copy(search = state.search.copy(selection = toggle(state.search.selection, intent.address)))

                MemoryTab.Saved ->
                    state.copy(saved = state.saved.copy(selection = toggle(state.saved.selection, intent.address)))

                MemoryTab.Page ->
                    state.copy(page = state.page.copy(selection = toggle(state.page.selection, intent.address)))
            }

            is MemoryWorkspaceIntent.ClearSelection -> when (intent.tab) {
                MemoryTab.Search -> state.copy(search = state.search.copy(selection = emptySet()))
                MemoryTab.Saved -> state.copy(saved = state.saved.copy(selection = emptySet()))
                MemoryTab.Page -> state.copy(page = state.page.copy(selection = emptySet()))
            }

            is MemoryWorkspaceIntent.AddToSaved -> {
                if (intent.addresses.isEmpty()) {
                    state
                } else {
                    val nextEntries = LinkedHashMap(state.saved.entries)
                    intent.addresses.toList().sorted().forEach { address ->
                        nextEntries.putIfAbsent(address, SavedMemoryEntry(address = address))
                    }
                    state.copy(saved = state.saved.copy(entries = nextEntries))
                }
            }

            is MemoryWorkspaceIntent.RemoveFromSaved -> {
                if (intent.address !in state.saved.entries) {
                    state
                } else {
                    val nextEntries = LinkedHashMap(state.saved.entries)
                    nextEntries.remove(intent.address)
                    state.copy(
                        saved = state.saved.copy(
                            entries = nextEntries,
                            selection = state.saved.selection - intent.address,
                        ),
                    )
                }
            }
        }

    private fun toggle(selection: Set<ULong>, address: ULong): Set<ULong> =
        if (address in selection) selection - address else selection + address
}
