package com.smlc666.lkmdbg.domain.session

import com.smlc666.lkmdbg.domain.gateway.SessionGateway
import com.smlc666.lkmdbg.domain.gateway.SessionGatewayResult
import com.smlc666.lkmdbg.domain.gateway.SessionGatewayState
import com.smlc666.lkmdbg.overlay.presentation.workspace.SessionUiState
import com.smlc666.lkmdbg.overlay.presentation.workspace.WorkspaceUiState
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
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

    @Test
    fun connectAndOpen_connectError_clearsFlagsAndDoesNotOpen() = runBlocking {
        val useCases = SessionUseCases(
            object : SessionGateway {
                override fun currentState(): SessionGatewayState = SessionGatewayState(
                    isConnected = false,
                    isSessionOpen = false,
                    message = "connect failed",
                )

                override suspend fun connect(): SessionGatewayResult =
                    SessionGatewayResult.Error("connect failed")

                override suspend fun openSession(): SessionGatewayResult =
                    error("openSession should not be called when connect fails")
            },
        )

        val seeded = WorkspaceUiState.initial().copy(
            session = SessionUiState(
                isConnected = true,
                isSessionOpen = true,
                message = "stale",
            ),
        )
        val next = useCases.connectAndOpen(seeded)

        assertFalse(next.session.isConnected)
        assertFalse(next.session.isSessionOpen)
        assertEquals("connect failed", next.session.message)
    }

    @Test
    fun connectAndOpen_openError_clearsSessionOpen() = runBlocking {
        val useCases = SessionUseCases(
            object : SessionGateway {
                override fun currentState(): SessionGatewayState = SessionGatewayState(
                    isConnected = true,
                    isSessionOpen = false,
                    message = "connected",
                )

                override suspend fun connect(): SessionGatewayResult =
                    SessionGatewayResult.Ok(currentState())

                override suspend fun openSession(): SessionGatewayResult =
                    SessionGatewayResult.Error("open failed")
            },
        )

        val next = useCases.connectAndOpen(WorkspaceUiState.initial())

        assertFalse(next.session.isConnected)
        assertFalse(next.session.isSessionOpen)
        assertEquals("open failed", next.session.message)
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
