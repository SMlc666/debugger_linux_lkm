package com.smlc666.lkmdbg.data.gateway

import com.smlc666.lkmdbg.data.bridge.SessionBridgeClient
import com.smlc666.lkmdbg.domain.gateway.SessionGatewayResult
import com.smlc666.lkmdbg.shared.BridgeHelloReply
import com.smlc666.lkmdbg.shared.BridgeOpenSessionReply
import com.smlc666.lkmdbg.shared.BridgeStatusCode
import com.smlc666.lkmdbg.shared.BridgeStatusSnapshot
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class SessionGatewayImplTest {
    @Test
    fun openSession_mapsInvalidHeaderToFailureResult() = runBlocking {
        val bridge = FakeSessionBridgeClient(
            openStatus = BridgeStatusCode.InvalidHeader.wireValue,
        )
        val gateway = SessionGatewayImpl(bridge)

        val result = gateway.openSession()

        assertTrue(result is SessionGatewayResult.Error)
        assertTrue((result as SessionGatewayResult.Error).message.contains("InvalidHeader"))
    }

    @Test
    fun connect_connectedSnapshot_mapsToOk() = runBlocking {
        val bridge = FakeSessionBridgeClient(
            connectStatus = BridgeStatusCode.Ok.wireValue,
            snapshot = statusSnapshot(connected = true, sessionOpen = false, message = "connected"),
        )
        val gateway = SessionGatewayImpl(bridge)

        val result = gateway.connect()

        assertTrue(result is SessionGatewayResult.Ok)
        val state = (result as SessionGatewayResult.Ok).state
        assertTrue(state.isConnected)
        assertFalse(state.isSessionOpen)
        assertEquals("connected", state.message)
    }

    @Test
    fun openSession_snapshotWithoutSessionOpen_mapsToError() = runBlocking {
        val bridge = FakeSessionBridgeClient(
            openStatus = BridgeStatusCode.Ok.wireValue,
            snapshot = statusSnapshot(connected = true, sessionOpen = false, message = "not open"),
        )
        val gateway = SessionGatewayImpl(bridge)

        val result = gateway.openSession()

        assertTrue(result is SessionGatewayResult.Error)
        assertEquals("not open", (result as SessionGatewayResult.Error).message)
    }

    private class FakeSessionBridgeClient(
        private val connectStatus: Int = BridgeStatusCode.Ok.wireValue,
        private val openStatus: Int = BridgeStatusCode.Ok.wireValue,
        private val snapshot: BridgeStatusSnapshot = statusSnapshot(),
    ) : SessionBridgeClient {
        override suspend fun connect(): BridgeHelloReply = BridgeHelloReply(
            status = connectStatus,
            serverVersion = 1u,
            featureBits = 0uL,
            message = "connect",
        )

        override suspend fun openSession(): BridgeOpenSessionReply = BridgeOpenSessionReply(
            status = openStatus,
            sessionOpen = openStatus == BridgeStatusCode.Ok.wireValue,
            sessionId = 1uL,
            message = "open",
        )

        override suspend fun statusSnapshot(): BridgeStatusSnapshot = snapshot
    }

    companion object {
        private fun statusSnapshot(
            connected: Boolean = true,
            sessionOpen: Boolean = true,
            message: String = "ok",
        ): BridgeStatusSnapshot = BridgeStatusSnapshot(
            status = BridgeStatusCode.Ok.wireValue,
            connected = connected,
            targetPid = 0,
            targetTid = 0,
            sessionOpen = sessionOpen,
            agentPid = 0,
            ownerPid = 0,
            hookActive = 0,
            eventQueueDepth = 0u,
            sessionId = 1uL,
            transport = "stdio-pipe",
            message = message,
        )
    }
}
