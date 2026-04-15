package com.smlc666.lkmdbg.overlay.presentation.memory

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update

class MemoryWorkspaceViewModel(
    initialState: MemoryWorkspaceState,
) {
    private val mutableState = MutableStateFlow(initialState)

    val state: StateFlow<MemoryWorkspaceState> = mutableState.asStateFlow()

    fun dispatch(intent: MemoryWorkspaceIntent) {
        mutableState.update { current ->
            MemoryWorkspaceReducer.reduce(current, intent)
        }
    }
}
