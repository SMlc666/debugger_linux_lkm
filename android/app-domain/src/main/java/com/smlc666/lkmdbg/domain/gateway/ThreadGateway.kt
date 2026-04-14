package com.smlc666.lkmdbg.domain.gateway

import com.smlc666.lkmdbg.shared.BridgeThreadRegistersReply

data class ThreadRecord(
    val tid: Int,
    val tgid: Int,
    val comm: String,
)

data class ThreadGatewayState(
    val threads: List<ThreadRecord>,
    val selectedTid: Int?,
    val selectedRegisters: BridgeThreadRegistersReply?,
    val message: String,
)

sealed interface ThreadGatewayResult {
    data class Ok(val state: ThreadGatewayState) : ThreadGatewayResult

    data class Error(val message: String) : ThreadGatewayResult
}

interface ThreadGateway {
    fun currentState(): ThreadGatewayState

    suspend fun refreshThreads(): ThreadGatewayResult

    suspend fun selectThread(tid: Int): ThreadGatewayResult
}

