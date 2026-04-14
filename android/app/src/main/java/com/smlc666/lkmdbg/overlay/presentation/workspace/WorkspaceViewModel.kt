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
                    mutableState.value = next
                }
            }
            is WorkspaceIntent.TogglePinnedEvent -> {
                scope.launch {
                    val next = eventUseCases.togglePinnedEvent(mutableState.value, intent.seq)
                    mutableState.value = next
                }
            }
        }
    }
}
