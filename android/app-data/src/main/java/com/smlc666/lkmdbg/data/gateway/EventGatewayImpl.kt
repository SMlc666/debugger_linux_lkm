package com.smlc666.lkmdbg.data.gateway

import com.smlc666.lkmdbg.data.SessionBridgeRepository
import com.smlc666.lkmdbg.domain.gateway.EventEntry
import com.smlc666.lkmdbg.domain.gateway.EventGateway
import com.smlc666.lkmdbg.domain.gateway.EventGatewayResult
import com.smlc666.lkmdbg.domain.gateway.EventGatewayState

class EventGatewayImpl(
    private val repository: SessionBridgeRepository,
) : EventGateway {
    override fun currentState(): EventGatewayState {
        val state = repository.state.value
        return EventGatewayState(
            events = state.recentEvents.map { entry ->
                EventEntry(
                    record = entry.record,
                    receivedAtMs = entry.receivedAtMs,
                )
            },
            pinnedEventSeqs = state.pinnedEventSeqs,
            message = state.lastMessage,
        )
    }

    override suspend fun refreshEvents(timeoutMs: Int, maxEvents: Int): EventGatewayResult {
        val before = repository.state.value
        repository.refreshEvents(timeoutMs, maxEvents)
        val after = repository.state.value
        val snapshot = currentState()

        // SessionBridgeRepository swallows transport errors. Refresh succeeds iff the
        // underlying event slice was updated.
        return if (after.recentEvents !== before.recentEvents) {
            EventGatewayResult.Ok(snapshot)
        } else {
            EventGatewayResult.Error(snapshot.message)
        }
    }

    override suspend fun togglePinnedEvent(seq: ULong): EventGatewayResult {
        val state = repository.state.value
        if (state.recentEvents.none { it.record.seq == seq }) {
            return EventGatewayResult.Error("event not found: seq=$seq")
        }
        repository.togglePinnedEvent(seq)
        return EventGatewayResult.Ok(currentState())
    }
}

