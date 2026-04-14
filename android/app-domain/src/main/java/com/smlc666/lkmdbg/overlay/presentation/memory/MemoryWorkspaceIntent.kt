package com.smlc666.lkmdbg.overlay.presentation.memory

sealed interface MemoryWorkspaceIntent {
    data class SwitchTab(val tab: MemoryTab) : MemoryWorkspaceIntent

    data class SetFilter(val tab: MemoryTab, val text: String) : MemoryWorkspaceIntent

    data class ToggleSelected(val tab: MemoryTab, val address: ULong) : MemoryWorkspaceIntent

    data class ClearSelection(val tab: MemoryTab) : MemoryWorkspaceIntent

    data class AddToSaved(val addresses: Set<ULong>) : MemoryWorkspaceIntent

    data class RemoveFromSaved(val address: ULong) : MemoryWorkspaceIntent
}
