package com.smlc666.lkmdbg.data.gateway

import com.smlc666.lkmdbg.data.MemorySearchUiState
import com.smlc666.lkmdbg.data.SessionBridgeState
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class MemoryGatewayImplTest {
    @Test
    fun didMemorySearchMakeProgress_snapshotOnly_countsAsProgressEvenWhenResultsReferenceUnchanged() {
        val before = SessionBridgeState(
            agentPath = "agent",
            lastMessage = "idle",
            memorySearch = MemorySearchUiState(
                snapshotReady = false,
                summary = "",
                results = emptyList(),
            ),
        )
        val after = before.copy(
            lastMessage = "snapshot captured",
            memorySearch = before.memorySearch.copy(
                snapshotReady = true,
                summary = "snapshot captured",
                results = emptyList(),
            ),
        )

        assertTrue(didMemorySearchMakeProgress(before, after))
        assertTrue(after.memorySearch.results === before.memorySearch.results)
    }

    @Test
    fun didMemorySearchMakeProgress_messageOnly_doesNotCountAsProgress() {
        val before = SessionBridgeState(
            agentPath = "agent",
            lastMessage = "idle",
            memorySearch = MemorySearchUiState(
                snapshotReady = false,
                summary = "",
                results = emptyList(),
            ),
        )
        val after = before.copy(lastMessage = "bridge error: something failed")

        assertTrue(after.lastMessage != before.lastMessage)
        assertTrue(after.memorySearch == before.memorySearch)
        assertTrue(after.memorySearch.results === before.memorySearch.results)
        assertFalse(didMemorySearchMakeProgress(before, after))
    }
}
