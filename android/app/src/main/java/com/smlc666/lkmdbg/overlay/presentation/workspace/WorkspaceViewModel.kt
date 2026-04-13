package com.smlc666.lkmdbg.overlay.presentation.workspace

import com.smlc666.lkmdbg.data.WorkspaceSection
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update

class WorkspaceViewModel(initialState: WorkspaceUiState) {
    private val mutableState = MutableStateFlow(initialState)

    val state: StateFlow<WorkspaceUiState> = mutableState.asStateFlow()

    fun dispatch(intent: WorkspaceIntent) {
        val action = when (intent) {
            is WorkspaceIntent.SelectSection -> WorkspaceAction.SetSection(intent.section)
            is WorkspaceIntent.SelectThread -> {
                // Selecting a thread implies navigating to the thread section, but the UI state does not
                // currently model the selected tid. Keep the intent separate for future use.
                WorkspaceAction.SetSection(WorkspaceSection.Threads)
            }
        }
        mutableState.update { current -> WorkspaceReducer.reduce(current, action) }
    }
}
}
