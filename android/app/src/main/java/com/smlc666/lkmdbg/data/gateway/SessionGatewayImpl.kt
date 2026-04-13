package com.smlc666.lkmdbg.data.gateway

import com.smlc666.lkmdbg.data.SessionBridgeRepository
import com.smlc666.lkmdbg.domain.gateway.SessionGateway
import com.smlc666.lkmdbg.domain.gateway.SessionGatewayResult
import com.smlc666.lkmdbg.domain.gateway.SessionGatewayState

class SessionGatewayImpl(
    private val repository: SessionBridgeRepository,
) : SessionGateway {
    override fun currentState(): SessionGatewayState {
        val state = repository.state.value
        return SessionGatewayState(
            isConnected = state.hello != null,
            isSessionOpen = state.snapshot.sessionOpen,
            message = state.lastMessage,
        )
    }

    override suspend fun connect(): SessionGatewayResult {
        repository.connect()
        val snapshot = currentState()
        return if (snapshot.isConnected) {
            SessionGatewayResult.Ok(snapshot)
        } else {
            SessionGatewayResult.Error(snapshot.message)
        }
    }

    override suspend fun openSession(): SessionGatewayResult {
        repository.openSession()
        val snapshot = currentState()
        return if (snapshot.isSessionOpen) {
            SessionGatewayResult.Ok(snapshot)
        } else {
            SessionGatewayResult.Error(snapshot.message)
        }
    }
}
