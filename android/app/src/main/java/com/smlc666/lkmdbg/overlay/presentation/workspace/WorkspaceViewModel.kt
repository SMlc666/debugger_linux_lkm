package com.smlc666.lkmdbg.overlay.presentation.workspace

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

class WorkspaceViewModel(initialState: WorkspaceUiState) {
    private val mutableState = MutableStateFlow(initialState)

    val state: StateFlow<WorkspaceUiState> = mutableState.asStateFlow()

    fun dispatch(intent: WorkspaceIntent) {
        val action = when (intent) {
            is WorkspaceIntent.SelectSection -> WorkspaceAction.SetSection(intent.section)
        }
        mutableState.value = WorkspaceReducer.reduce(mutableState.value, action)
    }
}
