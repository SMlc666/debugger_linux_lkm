package com.smlc666.lkmdbg.data

import android.content.Context
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.smlc666.lkmdbg.shared.BridgeHelloReply
import com.smlc666.lkmdbg.shared.BridgeOpenSessionReply
import com.smlc666.lkmdbg.shared.BridgeStatusSnapshot
import java.io.IOException

data class SessionBridgeState(
    val busy: Boolean = false,
    val agentPath: String,
    val targetPidInput: String = "",
    val hello: BridgeHelloReply? = null,
    val snapshot: BridgeStatusSnapshot = BridgeStatusSnapshot(
        status = 0,
        connected = false,
        targetPid = 0,
        targetTid = 0,
        sessionOpen = false,
        agentPid = 0,
        ownerPid = 0,
        hookActive = 0,
        eventQueueDepth = 0u,
        sessionId = 0u,
        transport = "su->stdio-pipe",
        message = "idle",
    ),
    val lastMessage: String = "idle",
)

class SessionBridgeController(
    private val client: PipeAgentClient,
) {
    var state by mutableStateOf(
        SessionBridgeState(
            agentPath = client.agentPathHint,
        ),
    )
        private set

    constructor(context: Context) : this(PipeAgentClient(context))

    fun updateTargetPidInput(value: String) {
        state = state.copy(targetPidInput = value.filter { it.isDigit() })
    }

    suspend fun connect() {
        runOperation(
            success = { reply ->
                state = state.copy(
                    hello = reply,
                    lastMessage = reply.message,
                )
            },
        ) {
            client.connect()
        }
    }

    suspend fun openSession() {
        runOperation(
            success = { reply: BridgeOpenSessionReply ->
                state = state.copy(
                    lastMessage = reply.message,
                )
            },
        ) {
            client.openSession()
        }
    }

    suspend fun refreshStatus() {
        runOperation(
            success = { snapshot: BridgeStatusSnapshot ->
                state = state.copy(
                    snapshot = snapshot,
                    lastMessage = snapshot.message,
                )
            },
        ) {
            client.statusSnapshot()
        }
    }

    suspend fun attachTarget() {
        val targetPid = state.targetPidInput.toIntOrNull()
        if (targetPid == null || targetPid <= 0) {
            state = state.copy(lastMessage = "enter a valid pid")
            return
        }

        runOperation(
            success = { reply ->
                state = state.copy(
                    lastMessage = reply.message,
                )
            },
        ) {
            client.setTarget(targetPid)
        }
        refreshStatus()
    }

    private suspend fun <T> runOperation(
        success: (T) -> Unit,
        block: suspend () -> T,
    ) {
        state = state.copy(busy = true)
        try {
            success(block())
        } catch (e: IOException) {
            state = state.copy(lastMessage = "io error: ${e.message ?: "unknown"}")
        } catch (e: IllegalStateException) {
            state = state.copy(lastMessage = "bridge error: ${e.message ?: "unknown"}")
        } finally {
            state = state.copy(busy = false)
        }
    }
}
