package com.smlc666.lkmdbg.domain.session

import com.smlc666.lkmdbg.domain.gateway.SessionGateway
import com.smlc666.lkmdbg.domain.gateway.SessionGatewayResult
import com.smlc666.lkmdbg.overlay.presentation.workspace.WorkspaceUiState

class SessionUseCases(
    private val gateway: SessionGateway,
) {
    suspend fun connectAndOpen(state: WorkspaceUiState): WorkspaceUiState {
        val connected = gateway.connect()
        val afterConnect = state.applySessionResult(connected)
        if (connected is SessionGatewayResult.Error) {
            return afterConnect
        }

        val opened = gateway.openSession()
        return afterConnect.applySessionResult(opened)
    }

    private fun WorkspaceUiState.applySessionResult(result: SessionGatewayResult): WorkspaceUiState =
        when (result) {
            is SessionGatewayResult.Ok -> {
                copy(
                    session = session.copy(
                        isConnected = result.state.isConnected,
                        isSessionOpen = result.state.isSessionOpen,
                        message = result.state.message,
                    ),
                )
            }
            is SessionGatewayResult.Error -> {
                copy(
                    session = session.copy(
                        message = result.message,
                    ),
                )
            }
        }
}
