package com.smlc666.lkmdbg.data.gateway

import com.smlc666.lkmdbg.data.SessionBridgeRepository
import com.smlc666.lkmdbg.domain.gateway.ProcessGateway
import com.smlc666.lkmdbg.domain.gateway.ProcessGatewayResult
import com.smlc666.lkmdbg.domain.gateway.ProcessGatewayState
import com.smlc666.lkmdbg.domain.gateway.ProcessRecord

class ProcessGatewayImpl(
    private val repository: SessionBridgeRepository,
) : ProcessGateway {
    override fun currentState(): ProcessGatewayState {
        val state = repository.state.value
        return ProcessGatewayState(
            processes = state.processes.map { process ->
                ProcessRecord(
                    pid = process.pid,
                    displayName = process.displayName,
                )
            },
            message = state.lastMessage,
        )
    }

    override suspend fun refreshProcesses(): ProcessGatewayResult {
        repository.refreshProcesses()
        return ProcessGatewayResult.Ok(currentState())
    }
}
