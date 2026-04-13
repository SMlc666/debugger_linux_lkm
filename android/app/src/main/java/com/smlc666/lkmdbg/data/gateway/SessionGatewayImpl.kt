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
            isConnected = state.snapshot.connected,
            isSessionOpen = state.snapshot.sessionOpen,
            message = state.lastMessage,
        )
    }

    override suspend fun connect(): SessionGatewayResult {
        repository.connect()
        val afterConnect = repository.state.value
        if (afterConnect.hello == null) {
            // connect() swallows exceptions, so use the post-connect snapshot deterministically.
            return SessionGatewayResult.Error(afterConnect.lastMessage)
        }

        repository.refreshStatus()
        val snapshot = currentState()
        return if (snapshot.isConnected) SessionGatewayResult.Ok(snapshot) else SessionGatewayResult.Error(snapshot.message)
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
