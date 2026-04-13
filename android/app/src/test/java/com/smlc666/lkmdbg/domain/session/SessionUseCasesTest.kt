package com.smlc666.lkmdbg.domain.session

import com.smlc666.lkmdbg.domain.gateway.SessionGateway
import com.smlc666.lkmdbg.domain.gateway.SessionGatewayResult
import com.smlc666.lkmdbg.domain.gateway.SessionGatewayState
import com.smlc666.lkmdbg.overlay.presentation.workspace.WorkspaceUiState
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class SessionUseCasesTest {
    @Test
    fun connectAndOpen_updatesSessionOpenAndMessage() = runBlocking {
        val useCases = SessionUseCases(FakeSessionGateway())

        val next = useCases.connectAndOpen(WorkspaceUiState.initial())

        assertTrue(next.session.isConnected)
        assertTrue(next.session.isSessionOpen)
        assertEquals("opened", next.session.message)
    }

    private class FakeSessionGateway : SessionGateway {
        private var current = SessionGatewayState(
            isConnected = false,
            isSessionOpen = false,
            message = "",
        )

        override fun currentState(): SessionGatewayState = current

        override suspend fun connect(): SessionGatewayResult {
            current = current.copy(
                isConnected = true,
                message = "connected",
            )
            return SessionGatewayResult.Ok(current)
        }

        override suspend fun openSession(): SessionGatewayResult {
            current = current.copy(
                isSessionOpen = true,
                message = "opened",
            )
            return SessionGatewayResult.Ok(current)
        }
    }
}
