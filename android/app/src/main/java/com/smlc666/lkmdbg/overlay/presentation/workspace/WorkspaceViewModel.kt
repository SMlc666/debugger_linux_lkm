package com.smlc666.lkmdbg.overlay.presentation.workspace

import com.smlc666.lkmdbg.domain.event.EventUseCases
import com.smlc666.lkmdbg.domain.thread.ThreadUseCases
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch

class WorkspaceViewModel(
    initialState: WorkspaceUiState,
    private val threadUseCases: ThreadUseCases,
    private val eventUseCases: EventUseCases,
    private val scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.Default),
) {
    private val mutableState = MutableStateFlow(initialState)

    val state: StateFlow<WorkspaceUiState> = mutableState.asStateFlow()

    fun dispatch(intent: WorkspaceIntent) {
        when (intent) {
            is WorkspaceIntent.SelectSection -> {
                val action = WorkspaceAction.SetSection(intent.section)
                mutableState.update { current -> WorkspaceReducer.reduce(current, action) }
            }
            is WorkspaceIntent.SelectThread -> {
                scope.launch {
                    val next = threadUseCases.selectThread(mutableState.value, intent.tid)
                    // Only merge the slice this intent owns. The usecase returns a full WorkspaceUiState
                    // derived from a snapshot, so assigning it wholesale can roll back concurrent updates.
                    mutableState.update { current -> current.copy(threads = next.threads) }
                }
            }
            is WorkspaceIntent.TogglePinnedEvent -> {
                scope.launch {
                    val next = eventUseCases.togglePinnedEvent(mutableState.value, intent.seq)
                    // See note above: only merge the relevant slice to avoid clobbering concurrent updates.
                    mutableState.update { current -> current.copy(events = next.events) }
                }
            }
        }
    }
}
