package com.smlc666.lkmdbg.domain.memory

import com.smlc666.lkmdbg.data.MemoryPage
import com.smlc666.lkmdbg.data.MemorySearchResult
import com.smlc666.lkmdbg.domain.gateway.MemoryGateway
import com.smlc666.lkmdbg.domain.gateway.MemoryGatewayResult

sealed interface MemoryUseCaseResult<out T> {
    data class Ok<T>(
        val value: T,
        val message: String,
    ) : MemoryUseCaseResult<T>

    data class Error(val message: String) : MemoryUseCaseResult<Nothing>
}

data class MemorySearchSnapshot(
    val snapshotReady: Boolean,
    val summary: String,
    val results: List<MemorySearchResult>,
)

class MemoryUseCases(
    private val gateway: MemoryGateway,
) {
    suspend fun openMemoryPage(remoteAddr: ULong): MemoryUseCaseResult<MemoryPage> =
        gateway.openMemoryPage(remoteAddr).toPageResult()

    suspend fun selectMemoryAddress(remoteAddr: ULong): MemoryUseCaseResult<MemoryPage> =
        gateway.selectMemoryAddress(remoteAddr).toPageResult()

    suspend fun writeHexAtFocus(): MemoryUseCaseResult<MemoryPage> =
        gateway.writeHexAtFocus().toPageResult()

    suspend fun writeAsciiAtFocus(): MemoryUseCaseResult<MemoryPage> =
        gateway.writeAsciiAtFocus().toPageResult()

    suspend fun runMemorySearch(): MemoryUseCaseResult<MemorySearchSnapshot> =
        gateway.runMemorySearch().toSearchResult()

    suspend fun refineMemorySearch(): MemoryUseCaseResult<MemorySearchSnapshot> =
        gateway.refineMemorySearch().toSearchResult()

    private fun MemoryGatewayResult.toPageResult(): MemoryUseCaseResult<MemoryPage> =
        when (this) {
            is MemoryGatewayResult.Ok -> {
                val page = state.page
                if (page != null) {
                    MemoryUseCaseResult.Ok(page, state.message)
                } else {
                    MemoryUseCaseResult.Error(state.message.ifBlank { "memory page unavailable" })
                }
            }

            is MemoryGatewayResult.Error -> MemoryUseCaseResult.Error(message)
        }

    private fun MemoryGatewayResult.toSearchResult(): MemoryUseCaseResult<MemorySearchSnapshot> =
        when (this) {
            is MemoryGatewayResult.Ok -> MemoryUseCaseResult.Ok(
                MemorySearchSnapshot(
                    snapshotReady = state.search.snapshotReady,
                    summary = state.search.summary,
                    results = state.search.results,
                ),
                state.message,
            )

            is MemoryGatewayResult.Error -> MemoryUseCaseResult.Error(message)
        }
}

