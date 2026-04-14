package com.smlc666.lkmdbg.domain.process

import com.smlc666.lkmdbg.domain.gateway.ProcessGateway
import com.smlc666.lkmdbg.domain.gateway.ProcessGatewayResult
import com.smlc666.lkmdbg.domain.gateway.ProcessGatewayState
import com.smlc666.lkmdbg.domain.gateway.ProcessRecord
import com.smlc666.lkmdbg.overlay.presentation.workspace.WorkspaceUiState
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Test

class ProcessUseCasesTest {
    @Test
    fun refreshProcesses_updatesWorkspaceProcessSlice() = runBlocking {
        val useCases = ProcessUseCases(
            FakeProcessGateway(
                processes = listOf(
                    ProcessRecord(
                        pid = 123,
                        displayName = "com.example.app",
                    ),
                ),
                message = "refreshed",
            ),
        )

        val next = useCases.refreshProcesses(WorkspaceUiState.initial())

        assertEquals(1, next.processes.items.size)
        assertEquals(123, next.processes.items.single().pid)
        assertEquals("refreshed", next.processes.message)
    }

    @Test
    fun refreshProcesses_error_updatesMessageWithoutTouchingItems() = runBlocking {
        val useCases = ProcessUseCases(
            object : ProcessGateway {
                override fun currentState(): ProcessGatewayState = ProcessGatewayState(
                    processes = emptyList(),
                    message = "error",
                )

                override suspend fun refreshProcesses(): ProcessGatewayResult =
                    ProcessGatewayResult.Error("error")
            },
        )

        val seeded = WorkspaceUiState.initial().copy(
            processes = WorkspaceUiState.initial().processes.copy(
                items = listOf(
                    ProcessRecord(
                        pid = 7,
                        displayName = "seed",
                    ),
                ),
                message = "stale",
            ),
        )

        val next = useCases.refreshProcesses(seeded)

        assertEquals(1, next.processes.items.size)
        assertEquals(7, next.processes.items.single().pid)
        assertEquals("error", next.processes.message)
    }

    private class FakeProcessGateway(
        private val processes: List<ProcessRecord>,
        private val message: String,
    ) : ProcessGateway {
        override fun currentState(): ProcessGatewayState = ProcessGatewayState(
            processes = processes,
            message = message,
        )

        override suspend fun refreshProcesses(): ProcessGatewayResult =
            ProcessGatewayResult.Ok(currentState())
    }
}
