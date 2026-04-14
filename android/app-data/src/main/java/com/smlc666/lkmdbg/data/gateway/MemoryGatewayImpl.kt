package com.smlc666.lkmdbg.data.gateway

import com.smlc666.lkmdbg.data.SessionBridgeRepository
import com.smlc666.lkmdbg.data.SessionBridgeState
import com.smlc666.lkmdbg.domain.gateway.MemoryGateway
import com.smlc666.lkmdbg.domain.gateway.MemoryGatewayResult
import com.smlc666.lkmdbg.domain.gateway.MemoryGatewayState
import com.smlc666.lkmdbg.domain.gateway.MemorySearchGatewayState

internal fun didMemorySearchMakeProgress(
    before: SessionBridgeState,
    after: SessionBridgeState,
): Boolean {
    val beforeSearch = before.memorySearch
    val afterSearch = after.memorySearch
    return afterSearch.snapshotReady != beforeSearch.snapshotReady ||
        afterSearch.summary != beforeSearch.summary ||
        afterSearch.results !== beforeSearch.results
}

class MemoryGatewayImpl(
    private val repository: SessionBridgeRepository,
) : MemoryGateway {
    override fun currentState(): MemoryGatewayState {
        val state = repository.state.value
        val search = state.memorySearch
        return MemoryGatewayState(
            page = state.memoryPage,
            search = MemorySearchGatewayState(
                snapshotReady = search.snapshotReady,
                summary = search.summary,
                results = search.results,
            ),
            message = state.lastMessage,
        )
    }

    override suspend fun openMemoryPage(remoteAddr: ULong): MemoryGatewayResult {
        val before = repository.state.value
        repository.openMemoryPage(remoteAddr)
        val after = repository.state.value
        val snapshot = currentState()
        return if (after.memoryPage !== before.memoryPage) {
            MemoryGatewayResult.Ok(snapshot)
        } else {
            MemoryGatewayResult.Error(snapshot.message)
        }
    }

    override suspend fun selectMemoryAddress(remoteAddr: ULong): MemoryGatewayResult {
        val before = repository.state.value
        repository.selectMemoryAddress(remoteAddr)
        val after = repository.state.value
        val snapshot = currentState()
        return if (after.memoryPage !== before.memoryPage) {
            MemoryGatewayResult.Ok(snapshot)
        } else {
            MemoryGatewayResult.Error(snapshot.message)
        }
    }

    override suspend fun writeHexAtFocus(): MemoryGatewayResult {
        val before = repository.state.value
        repository.writeHexAtFocus()
        val after = repository.state.value
        val snapshot = currentState()
        return if (after.memoryPage !== before.memoryPage) {
            MemoryGatewayResult.Ok(snapshot)
        } else {
            MemoryGatewayResult.Error(snapshot.message)
        }
    }

    override suspend fun writeAsciiAtFocus(): MemoryGatewayResult {
        val before = repository.state.value
        repository.writeAsciiAtFocus()
        val after = repository.state.value
        val snapshot = currentState()
        return if (after.memoryPage !== before.memoryPage) {
            MemoryGatewayResult.Ok(snapshot)
        } else {
            MemoryGatewayResult.Error(snapshot.message)
        }
    }

    override suspend fun runMemorySearch(): MemoryGatewayResult {
        val before = repository.state.value
        repository.runMemorySearch()
        val after = repository.state.value
        val snapshot = currentState()
        return if (didMemorySearchMakeProgress(before, after)) {
            MemoryGatewayResult.Ok(snapshot)
        } else {
            MemoryGatewayResult.Error(snapshot.message)
        }
    }

    override suspend fun refineMemorySearch(): MemoryGatewayResult {
        val before = repository.state.value
        repository.refineMemorySearch()
        val after = repository.state.value
        val snapshot = currentState()
        return if (didMemorySearchMakeProgress(before, after)) {
            MemoryGatewayResult.Ok(snapshot)
        } else {
            MemoryGatewayResult.Error(snapshot.message)
        }
    }
}
