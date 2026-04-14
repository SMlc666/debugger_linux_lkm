package com.smlc666.lkmdbg.domain.event

import com.smlc666.lkmdbg.domain.gateway.EventEntry
import com.smlc666.lkmdbg.domain.gateway.EventGateway
import com.smlc666.lkmdbg.domain.gateway.EventGatewayResult
import com.smlc666.lkmdbg.domain.gateway.EventGatewayState
import com.smlc666.lkmdbg.overlay.presentation.workspace.WorkspaceUiState
import com.smlc666.lkmdbg.shared.BridgeEventRecord
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class EventUseCasesTest {
    @Test
    fun togglePinnedEvent_updatesPinsAndSortOrder() = runBlocking {
        val events = listOf(
            entry(seq = 2uL, receivedAtMs = 20),
            entry(seq = 1uL, receivedAtMs = 30),
        )
        val useCases = EventUseCases(
            FakeEventGateway(
                events = events,
                pinnedEventSeqs = emptySet(),
                message = "idle",
            ),
        )

        val next = useCases.togglePinnedEvent(WorkspaceUiState.initial(), seq = 1uL)

        assertTrue(1uL in next.events.pinnedEventSeqs)
        assertEquals(1uL, next.events.items.first().record.seq)
        assertEquals(2uL, next.events.items[1].record.seq)
        assertEquals("toggled 1", next.events.message)
    }

    @Test
    fun togglePinnedEvent_error_updatesMessageWithoutTouchingItemsOrPins() = runBlocking {
        val useCases = EventUseCases(
            object : EventGateway {
                override fun currentState(): EventGatewayState = EventGatewayState(
                    events = emptyList(),
                    pinnedEventSeqs = emptySet(),
                    message = "error",
                )

                override suspend fun refreshEvents(timeoutMs: Int, maxEvents: Int): EventGatewayResult =
                    EventGatewayResult.Error("error")

                override suspend fun togglePinnedEvent(seq: ULong): EventGatewayResult =
                    EventGatewayResult.Error("error")
            },
        )

        val seeded = WorkspaceUiState.initial().copy(
            events = WorkspaceUiState.initial().events.copy(
                items = listOf(
                    entry(seq = 7uL, receivedAtMs = 10),
                ),
                pinnedEventSeqs = setOf(7uL),
                message = "stale",
            ),
        )

        val next = useCases.togglePinnedEvent(seeded, seq = 123uL)

        assertEquals(1, next.events.items.size)
        assertEquals(7uL, next.events.items.single().record.seq)
        assertEquals(setOf(7uL), next.events.pinnedEventSeqs)
        assertEquals("error", next.events.message)
    }

    private class FakeEventGateway(
        private val events: List<EventEntry>,
        pinnedEventSeqs: Set<ULong>,
        message: String,
    ) : EventGateway {
        private var current: EventGatewayState = EventGatewayState(
            events = events,
            pinnedEventSeqs = pinnedEventSeqs,
            message = message,
        )

        override fun currentState(): EventGatewayState = current

        override suspend fun refreshEvents(timeoutMs: Int, maxEvents: Int): EventGatewayResult =
            EventGatewayResult.Ok(currentState())

        override suspend fun togglePinnedEvent(seq: ULong): EventGatewayResult {
            val nextPins = if (seq in current.pinnedEventSeqs) {
                current.pinnedEventSeqs - seq
            } else {
                current.pinnedEventSeqs + seq
            }
            current = current.copy(
                pinnedEventSeqs = nextPins,
                message = "toggled $seq",
            )
            return EventGatewayResult.Ok(current)
        }
    }

    private fun entry(seq: ULong, receivedAtMs: Long): EventEntry =
        EventEntry(
            record = BridgeEventRecord(
                version = 1u,
                type = 0u,
                size = 0u,
                code = 0u,
                sessionId = 0uL,
                seq = seq,
                tgid = 0,
                tid = 0,
                flags = 0u,
                value0 = 0uL,
                value1 = 0uL,
            ),
            receivedAtMs = receivedAtMs,
        )
}

