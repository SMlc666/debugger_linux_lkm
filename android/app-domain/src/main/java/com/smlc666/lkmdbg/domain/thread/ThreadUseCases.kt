package com.smlc666.lkmdbg.domain.thread

import com.smlc666.lkmdbg.domain.gateway.ThreadGateway
import com.smlc666.lkmdbg.domain.gateway.ThreadGatewayResult
import com.smlc666.lkmdbg.overlay.presentation.workspace.WorkspaceUiState

class ThreadUseCases(
    private val gateway: ThreadGateway,
) {
    suspend fun selectThread(state: WorkspaceUiState, tid: Int): WorkspaceUiState {
        val result = gateway.selectThread(tid)
        return when (result) {
            is ThreadGatewayResult.Ok -> state.copy(
                threads = state.threads.copy(
                    items = result.state.threads,
                    selectedTid = result.state.selectedTid,
                    selectedRegisters = result.state.selectedRegisters,
                    message = result.state.message,
                ),
            )
            is ThreadGatewayResult.Error -> state.copy(
                threads = state.threads.copy(
                    message = result.message,
                ),
            )
        }
    }
}

