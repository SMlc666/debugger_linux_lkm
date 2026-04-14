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
        return when (opened) {
            is SessionGatewayResult.Ok -> afterConnect.applySessionResult(opened)
            is SessionGatewayResult.Error -> afterConnect.copy(
                session = afterConnect.session.copy(
                    isSessionOpen = false,
                    message = opened.message,
                ),
            )
        }
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
                        isConnected = false,
                        isSessionOpen = false,
                        message = result.message,
                    ),
                )
            }
        }
}
