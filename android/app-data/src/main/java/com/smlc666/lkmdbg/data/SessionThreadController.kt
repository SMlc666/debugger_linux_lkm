package com.smlc666.lkmdbg.data

import android.content.Context
import com.smlc666.lkmdbg.appdata.R
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.update

internal class SessionThreadController(
    context: Context,
    private val client: PipeAgentClient,
    private val stateFlow: MutableStateFlow<SessionBridgeState>,
) {
    private val appContext = context.applicationContext

    suspend fun refreshRegisters(tid: Int) {
        val reply = client.getRegisters(tid)
        ensureBridgeStatusOk(reply.status, reply.message, "GET_REGISTERS")
        stateFlow.update { current ->
            current.copy(
                selectedThreadTid = tid,
                selectedThreadRegisters = reply,
                lastMessage = reply.message.ifBlank {
                    appContext.getString(R.string.thread_message_registers, tid)
                },
            )
        }
    }

    fun clearSelection() {
        stateFlow.update { current ->
            current.copy(
                threads = emptyList(),
                selectedThreadTid = null,
                selectedThreadRegisters = null,
            )
        }
    }
}
