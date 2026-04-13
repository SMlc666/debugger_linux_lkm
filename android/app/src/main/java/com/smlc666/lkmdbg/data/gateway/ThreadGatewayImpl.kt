package com.smlc666.lkmdbg.data.gateway

import com.smlc666.lkmdbg.data.SessionBridgeRepository
import com.smlc666.lkmdbg.domain.gateway.ThreadGateway
import com.smlc666.lkmdbg.domain.gateway.ThreadGatewayResult
import com.smlc666.lkmdbg.domain.gateway.ThreadGatewayState
import com.smlc666.lkmdbg.domain.gateway.ThreadRecord

class ThreadGatewayImpl(
    private val repository: SessionBridgeRepository,
) : ThreadGateway {
    override fun currentState(): ThreadGatewayState {
        val state = repository.state.value
        return ThreadGatewayState(
            threads = state.threads.map { thread ->
                ThreadRecord(
                    tid = thread.tid,
                    tgid = thread.tgid,
                    comm = thread.comm,
                )
            },
            selectedTid = state.selectedThreadTid,
            selectedRegisters = state.selectedThreadRegisters,
            message = state.lastMessage,
        )
    }

    override suspend fun refreshThreads(): ThreadGatewayResult {
        val before = repository.state.value
        repository.refreshThreads()
        val after = repository.state.value
        val snapshot = currentState()

        // SessionBridgeRepository swallows transport errors. Refresh succeeds iff the
        // underlying thread list slice was updated.
        return if (after.threads !== before.threads) {
            ThreadGatewayResult.Ok(snapshot)
        } else {
            ThreadGatewayResult.Error(snapshot.message)
        }
    }

    override suspend fun selectThread(tid: Int): ThreadGatewayResult {
        repository.selectThread(tid)
        val snapshot = currentState()
        val registers = snapshot.selectedRegisters
        return if (snapshot.selectedTid == tid && registers != null && registers.tid == tid) {
            ThreadGatewayResult.Ok(snapshot)
        } else {
            ThreadGatewayResult.Error(snapshot.message)
        }
    }
}

