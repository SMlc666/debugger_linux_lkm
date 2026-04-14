package com.smlc666.lkmdbg.domain.gateway

import com.smlc666.lkmdbg.shared.BridgeEventRecord

data class EventEntry(
    val record: BridgeEventRecord,
    val receivedAtMs: Long,
)

data class EventGatewayState(
    val events: List<EventEntry>,
    val pinnedEventSeqs: Set<ULong>,
    val message: String,
)

sealed interface EventGatewayResult {
    data class Ok(val state: EventGatewayState) : EventGatewayResult

    data class Error(val message: String) : EventGatewayResult
}

interface EventGateway {
    fun currentState(): EventGatewayState

    suspend fun refreshEvents(timeoutMs: Int, maxEvents: Int): EventGatewayResult

    suspend fun togglePinnedEvent(seq: ULong): EventGatewayResult
}

