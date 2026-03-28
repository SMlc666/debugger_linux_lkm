package com.smlc666.lkmdbg.data

import android.content.Context
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.shared.BridgeHelloReply
import com.smlc666.lkmdbg.shared.BridgeOpenSessionReply
import com.smlc666.lkmdbg.shared.BridgeStatusSnapshot
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
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
    val lastMessage: String,
)

class SessionBridgeRepository(
    context: Context,
    private val client: PipeAgentClient = PipeAgentClient(context.applicationContext),
) {
    private val appContext = context.applicationContext
    private val _state = MutableStateFlow(
        SessionBridgeState(
            agentPath = client.agentPathHint,
            lastMessage = appContext.getString(R.string.session_message_idle),
        ),
    )

    val state: StateFlow<SessionBridgeState> = _state.asStateFlow()

    fun updateTargetPidInput(value: String) {
        _state.update { current ->
            current.copy(targetPidInput = value.filter { it.isDigit() })
        }
    }

    suspend fun connect() {
        runOperation(
            success = { reply: BridgeHelloReply ->
                _state.update { current ->
                    current.copy(
                        hello = reply,
                        lastMessage = reply.message.ifBlank {
                            appContext.getString(R.string.session_message_connected)
                        },
                    )
                }
            },
            block = client::connect,
        )
    }

    suspend fun openSession() {
        runOperation(
            success = { reply: BridgeOpenSessionReply ->
                _state.update { current ->
                    current.copy(
                        lastMessage = reply.message.ifBlank {
                            appContext.getString(R.string.session_message_opened)
                        },
                    )
                }
            },
            block = client::openSession,
        )
    }

    suspend fun refreshStatus() {
        runOperation(
            success = { snapshot: BridgeStatusSnapshot ->
                _state.update { current ->
                    current.copy(
                        snapshot = snapshot,
                        lastMessage = snapshot.message.ifBlank {
                            appContext.getString(R.string.session_message_refreshed)
                        },
                    )
                }
            },
            block = client::statusSnapshot,
        )
    }

    suspend fun attachTarget() {
        val targetPid = state.value.targetPidInput.toIntOrNull()
        if (targetPid == null || targetPid <= 0) {
            _state.update { current ->
                current.copy(lastMessage = appContext.getString(R.string.session_error_invalid_pid))
            }
            return
        }

        runOperation(
            success = { reply ->
                _state.update { current ->
                    current.copy(
                        lastMessage = reply.message.ifBlank {
                            appContext.getString(R.string.session_message_target_attached, targetPid)
                        },
                    )
                }
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
        _state.update { current -> current.copy(busy = true) }
        try {
            success(block())
        } catch (e: IOException) {
            _state.update { current ->
                current.copy(
                    lastMessage = appContext.getString(
                        R.string.session_error_io,
                        e.message ?: appContext.getString(R.string.session_error_unknown),
                    ),
                )
            }
        } catch (e: IllegalStateException) {
            _state.update { current ->
                current.copy(
                    lastMessage = appContext.getString(
                        R.string.session_error_bridge,
                        e.message ?: appContext.getString(R.string.session_error_unknown),
                    ),
                )
            }
        } finally {
            _state.update { current -> current.copy(busy = false) }
        }
    }
}
