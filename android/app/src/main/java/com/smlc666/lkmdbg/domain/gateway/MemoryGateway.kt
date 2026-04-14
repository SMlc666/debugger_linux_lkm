package com.smlc666.lkmdbg.domain.gateway

import com.smlc666.lkmdbg.data.MemoryPage
import com.smlc666.lkmdbg.data.MemorySearchResult

data class MemorySearchGatewayState(
    val snapshotReady: Boolean,
    val summary: String,
    val results: List<MemorySearchResult>,
)

data class MemoryGatewayState(
    val page: MemoryPage?,
    val search: MemorySearchGatewayState,
    val message: String,
)

sealed interface MemoryGatewayResult {
    data class Ok(val state: MemoryGatewayState) : MemoryGatewayResult

    data class Error(val message: String) : MemoryGatewayResult
}

interface MemoryGateway {
    fun currentState(): MemoryGatewayState

    suspend fun openMemoryPage(remoteAddr: ULong): MemoryGatewayResult

    suspend fun selectMemoryAddress(remoteAddr: ULong): MemoryGatewayResult

    suspend fun writeHexAtFocus(): MemoryGatewayResult

    suspend fun writeAsciiAtFocus(): MemoryGatewayResult

    suspend fun runMemorySearch(): MemoryGatewayResult

    suspend fun refineMemorySearch(): MemoryGatewayResult
}

