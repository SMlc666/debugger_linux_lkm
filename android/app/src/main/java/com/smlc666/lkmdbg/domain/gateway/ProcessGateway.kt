package com.smlc666.lkmdbg.domain.gateway

data class ProcessRecord(
    val pid: Int,
    val displayName: String,
)

data class ProcessGatewayState(
    val processes: List<ProcessRecord>,
    val message: String,
)

sealed interface ProcessGatewayResult {
    data class Ok(val state: ProcessGatewayState) : ProcessGatewayResult

    data class Error(val message: String) : ProcessGatewayResult
}

interface ProcessGateway {
    fun currentState(): ProcessGatewayState

    suspend fun refreshProcesses(): ProcessGatewayResult
}
