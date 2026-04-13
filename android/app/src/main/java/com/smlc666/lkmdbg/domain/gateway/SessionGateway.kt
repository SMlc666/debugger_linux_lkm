package com.smlc666.lkmdbg.domain.gateway

data class SessionGatewayState(
    val isConnected: Boolean,
    val isSessionOpen: Boolean,
    val message: String,
)

sealed interface SessionGatewayResult {
    data class Ok(val state: SessionGatewayState) : SessionGatewayResult

    data class Error(val message: String) : SessionGatewayResult
}

interface SessionGateway {
    fun currentState(): SessionGatewayState

    suspend fun connect(): SessionGatewayResult

    suspend fun openSession(): SessionGatewayResult
}
