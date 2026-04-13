package com.smlc666.lkmdbg.domain.process

import com.smlc666.lkmdbg.domain.gateway.ProcessGateway
import com.smlc666.lkmdbg.domain.gateway.ProcessGatewayResult
import com.smlc666.lkmdbg.overlay.presentation.workspace.WorkspaceUiState

class ProcessUseCases(
    private val gateway: ProcessGateway,
) {
    suspend fun refreshProcesses(state: WorkspaceUiState): WorkspaceUiState {
        val result = gateway.refreshProcesses()
        return when (result) {
            is ProcessGatewayResult.Ok -> state.copy(
                processes = state.processes.copy(
                    items = result.state.processes,
                    message = result.state.message,
                ),
            )
            is ProcessGatewayResult.Error -> state.copy(
                processes = state.processes.copy(
                    message = result.message,
                ),
            )
        }
    }
}
