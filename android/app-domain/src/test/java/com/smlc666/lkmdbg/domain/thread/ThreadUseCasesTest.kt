package com.smlc666.lkmdbg.domain.thread

import com.smlc666.lkmdbg.domain.gateway.ThreadGateway
import com.smlc666.lkmdbg.domain.gateway.ThreadGatewayResult
import com.smlc666.lkmdbg.domain.gateway.ThreadGatewayState
import com.smlc666.lkmdbg.domain.gateway.ThreadRecord
import com.smlc666.lkmdbg.overlay.presentation.workspace.WorkspaceUiState
import com.smlc666.lkmdbg.shared.BridgeThreadRegistersReply
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Test

class ThreadUseCasesTest {
    @Test
    fun selectThread_updatesSelectionAndRegisters() = runBlocking {
        val useCases = ThreadUseCases(
            FakeThreadGateway(
                threads = listOf(
                    ThreadRecord(
                        tid = 5,
                        tgid = 5,
                        comm = "main",
                    ),
                    ThreadRecord(
                        tid = 7,
                        tgid = 5,
                        comm = "worker",
                    ),
                ),
                message = "idle",
            ),
        )

        val next = useCases.selectThread(WorkspaceUiState.initial(), tid = 7)

        assertEquals(7, next.threads.selectedTid)
        assertNotNull(next.threads.selectedRegisters)
        assertEquals(7, next.threads.selectedRegisters?.tid)
        assertEquals("selected 7", next.threads.message)
    }

    @Test
    fun selectThread_error_updatesMessageWithoutTouchingSelection() = runBlocking {
        val useCases = ThreadUseCases(
            object : ThreadGateway {
                override fun currentState(): ThreadGatewayState = ThreadGatewayState(
                    threads = emptyList(),
                    selectedTid = null,
                    selectedRegisters = null,
                    message = "error",
                )

                override suspend fun refreshThreads(): ThreadGatewayResult =
                    ThreadGatewayResult.Error("error")

                override suspend fun selectThread(tid: Int): ThreadGatewayResult =
                    ThreadGatewayResult.Error("error")
            },
        )

        val seeded = WorkspaceUiState.initial().copy(
            threads = WorkspaceUiState.initial().threads.copy(
                items = listOf(
                    ThreadRecord(
                        tid = 7,
                        tgid = 7,
                        comm = "seed",
                    ),
                ),
                selectedTid = 7,
                selectedRegisters = buildRegisters(tid = 7, message = "seeded"),
                message = "stale",
            ),
        )

        val next = useCases.selectThread(seeded, tid = 123)

        assertEquals(7, next.threads.selectedTid)
        assertEquals(7, next.threads.selectedRegisters?.tid)
        assertEquals("error", next.threads.message)
    }

    private class FakeThreadGateway(
        private val threads: List<ThreadRecord>,
        private val message: String,
    ) : ThreadGateway {
        private var current: ThreadGatewayState = ThreadGatewayState(
            threads = threads,
            selectedTid = null,
            selectedRegisters = null,
            message = message,
        )

        override fun currentState(): ThreadGatewayState = current

        override suspend fun refreshThreads(): ThreadGatewayResult =
            ThreadGatewayResult.Ok(currentState())

        override suspend fun selectThread(tid: Int): ThreadGatewayResult {
            current = current.copy(
                selectedTid = tid,
                selectedRegisters = buildRegisters(tid = tid, message = "selected $tid"),
                message = "selected $tid",
            )
            return ThreadGatewayResult.Ok(current)
        }
    }
}

private fun buildRegisters(tid: Int, message: String): BridgeThreadRegistersReply =
    BridgeThreadRegistersReply(
        status = 0,
        tid = tid,
        flags = 0u,
        regs = ULongArray(31),
        sp = 0uL,
        pc = 0uL,
        pstate = 0uL,
        features = 0u,
        fpsr = 0u,
        fpcr = 0u,
        v0Lo = 0uL,
        v0Hi = 0uL,
        message = message,
    )
