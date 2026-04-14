package com.smlc666.lkmdbg.domain.event

import com.smlc666.lkmdbg.domain.gateway.EventEntry
import com.smlc666.lkmdbg.domain.gateway.EventGateway
import com.smlc666.lkmdbg.domain.gateway.EventGatewayResult
import com.smlc666.lkmdbg.overlay.presentation.workspace.WorkspaceUiState

class EventUseCases(
    private val gateway: EventGateway,
) {
    suspend fun togglePinnedEvent(state: WorkspaceUiState, seq: ULong): WorkspaceUiState {
        val result = gateway.togglePinnedEvent(seq)
        return when (result) {
            is EventGatewayResult.Ok -> {
                val pinned = result.state.pinnedEventSeqs
                state.copy(
                    events = state.events.copy(
                        items = sortEvents(result.state.events, pinned),
                        pinnedEventSeqs = pinned,
                        message = result.state.message,
                    ),
                )
            }
            is EventGatewayResult.Error -> state.copy(
                events = state.events.copy(
                    message = result.message,
                ),
            )
        }
    }

    private fun sortEvents(
        events: List<EventEntry>,
        pinnedEventSeqs: Set<ULong>,
    ): List<EventEntry> = events.sortedWith { left, right ->
        val leftPinned = left.record.seq in pinnedEventSeqs
        val rightPinned = right.record.seq in pinnedEventSeqs
        when {
            leftPinned != rightPinned -> if (leftPinned) -1 else 1
            left.record.seq != right.record.seq -> right.record.seq.compareTo(left.record.seq)
            else -> right.receivedAtMs.compareTo(left.receivedAtMs)
        }
    }
}

