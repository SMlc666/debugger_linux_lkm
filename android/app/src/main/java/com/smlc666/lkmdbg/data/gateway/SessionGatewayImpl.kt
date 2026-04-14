package com.smlc666.lkmdbg.data.gateway

import com.smlc666.lkmdbg.data.SessionBridgeRepository
import com.smlc666.lkmdbg.data.bridge.SessionBridgeClient
import com.smlc666.lkmdbg.domain.gateway.SessionGateway
import com.smlc666.lkmdbg.domain.gateway.SessionGatewayResult
import com.smlc666.lkmdbg.domain.gateway.SessionGatewayState
import com.smlc666.lkmdbg.shared.BridgeStatusCode
import com.smlc666.lkmdbg.shared.BridgeStatusSnapshot

class SessionGatewayImpl(
    private val bridgeClient: SessionBridgeClient,
) : SessionGateway {
    constructor(repository: SessionBridgeRepository) : this(repository.bridgeClient())

    private var current = SessionGatewayState(
        isConnected = false,
        isSessionOpen = false,
        message = "",
    )

    override fun currentState(): SessionGatewayState = current

    override suspend fun connect(): SessionGatewayResult {
        val hello = bridgeClient.connect()
        if (hello.status != BridgeStatusCode.Ok.wireValue) {
            return errorResult("connect", hello.status, hello.message)
        }

        val snapshot = bridgeClient.statusSnapshot()
        if (snapshot.status != BridgeStatusCode.Ok.wireValue) {
            return errorResult("statusSnapshot", snapshot.status, snapshot.message)
        }
        return applySnapshot(snapshot, requireSessionOpen = false)
    }

    override suspend fun openSession(): SessionGatewayResult {
        val open = bridgeClient.openSession()
        if (open.status != BridgeStatusCode.Ok.wireValue) {
            return errorResult("openSession", open.status, open.message)
        }

        val snapshot = bridgeClient.statusSnapshot()
        if (snapshot.status != BridgeStatusCode.Ok.wireValue) {
            return errorResult("statusSnapshot", snapshot.status, snapshot.message)
        }
        return applySnapshot(snapshot, requireSessionOpen = true)
    }

    private fun applySnapshot(
        snapshot: BridgeStatusSnapshot,
        requireSessionOpen: Boolean,
    ): SessionGatewayResult {
        current = SessionGatewayState(
            isConnected = snapshot.connected,
            isSessionOpen = snapshot.sessionOpen,
            message = snapshot.message,
        )
        val success = if (requireSessionOpen) {
            snapshot.connected && snapshot.sessionOpen
        } else {
            snapshot.connected
        }
        return if (success) {
            SessionGatewayResult.Ok(current)
        } else {
            SessionGatewayResult.Error(current.message)
        }
    }

    private fun errorResult(operation: String, status: Int, message: String): SessionGatewayResult {
        val statusLabel = BridgeStatusCode.entries
            .firstOrNull { it.wireValue == status }
            ?.name
            ?: "Status($status)"
        val mergedMessage = if (message.isBlank()) {
            "$operation failed: $statusLabel"
        } else {
            "$operation failed: $statusLabel ($message)"
        }
        current = current.copy(
            isConnected = false,
            isSessionOpen = false,
            message = mergedMessage,
        )
        return SessionGatewayResult.Error(mergedMessage)
    }
}
