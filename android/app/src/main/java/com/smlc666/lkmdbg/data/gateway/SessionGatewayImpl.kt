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
        val before = currentState()
        if (before.isConnected) {
            return SessionGatewayResult.Ok(before)
        }

        repository.connect()
        repository.refreshStatus()
        val after = currentState()
        return if (after.isConnected) SessionGatewayResult.Ok(after) else SessionGatewayResult.Error(after.message)
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
