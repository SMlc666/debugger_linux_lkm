package com.smlc666.lkmdbg.domain.memory

import com.smlc666.lkmdbg.data.MemoryPage
import com.smlc666.lkmdbg.data.MemorySearchResult
import com.smlc666.lkmdbg.data.MemorySearchUiState
import com.smlc666.lkmdbg.data.SessionBridgeState
import com.smlc666.lkmdbg.data.gateway.didMemorySearchMakeProgress
import com.smlc666.lkmdbg.domain.gateway.MemoryGateway
import com.smlc666.lkmdbg.domain.gateway.MemoryGatewayResult
import com.smlc666.lkmdbg.domain.gateway.MemoryGatewayState
import com.smlc666.lkmdbg.domain.gateway.MemorySearchGatewayState
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class MemoryUseCasesTest {
    @Test
    fun openMemoryPage_ok_returnsPage() = runBlocking {
        val useCases = MemoryUseCases(
            FakeMemoryGateway(
                nextResult = MemoryGatewayResult.Ok(
                    MemoryGatewayState(
                        page = page(focusAddress = 0x1000uL),
                        search = emptySearch(),
                        message = "opened",
                    ),
                ),
            ),
        )

        val result = useCases.openMemoryPage(remoteAddr = 0x1000uL)

        assertTrue(result is MemoryUseCaseResult.Ok)
        val ok = result as MemoryUseCaseResult.Ok
        assertEquals(0x1000uL, ok.value.focusAddress)
        assertEquals("opened", ok.message)
    }

    @Test
    fun openMemoryPage_okWithoutPage_mapsToError() = runBlocking {
        val useCases = MemoryUseCases(
            FakeMemoryGateway(
                nextResult = MemoryGatewayResult.Ok(
                    MemoryGatewayState(
                        page = null,
                        search = emptySearch(),
                        message = "no page",
                    ),
                ),
            ),
        )

        val result = useCases.openMemoryPage(remoteAddr = 0x1000uL)

        assertTrue(result is MemoryUseCaseResult.Error)
        assertEquals("no page", (result as MemoryUseCaseResult.Error).message)
    }

    @Test
    fun openMemoryPage_gatewayError_passesThroughMessage() = runBlocking {
        val useCases = MemoryUseCases(
            FakeMemoryGateway(
                nextResult = MemoryGatewayResult.Error("transport error"),
            ),
        )

        val result = useCases.openMemoryPage(remoteAddr = 0x1000uL)

        assertTrue(result is MemoryUseCaseResult.Error)
        assertEquals("transport error", (result as MemoryUseCaseResult.Error).message)
    }

    @Test
    fun runMemorySearch_ok_returnsSnapshot() = runBlocking {
        val hits = listOf(
            MemorySearchResult(
                address = 0x2000uL,
                regionName = "heap",
                regionStart = 0x1000uL,
                regionEnd = 0x3000uL,
                matchSize = 4,
                previewBytes = byteArrayOf(1.toByte(), 2.toByte(), 3.toByte(), 4.toByte()),
                previewHex = "01 02 03 04",
                valueSummary = "0x04030201",
            ),
        )
        val useCases = MemoryUseCases(
            FakeMemoryGateway(
                nextResult = MemoryGatewayResult.Ok(
                    MemoryGatewayState(
                        page = null,
                        search = MemorySearchGatewayState(
                            snapshotReady = true,
                            summary = "1 hits",
                            results = hits,
                        ),
                        message = "searched",
                    ),
                ),
            ),
        )

        val result = useCases.runMemorySearch()

        assertTrue(result is MemoryUseCaseResult.Ok)
        val ok = result as MemoryUseCaseResult.Ok
        assertTrue(ok.value.snapshotReady)
        assertEquals("1 hits", ok.value.summary)
        assertEquals(1, ok.value.results.size)
        assertEquals("searched", ok.message)
    }

    @Test
    fun runMemorySearch_snapshotOnly_ok_withEmptyResults() = runBlocking {
        val sharedEmptyResults = emptyList<MemorySearchResult>()
        val useCases = MemoryUseCases(
            FakeMemoryGateway(
                nextResult = MemoryGatewayResult.Ok(
                    MemoryGatewayState(
                        page = null,
                        search = MemorySearchGatewayState(
                            snapshotReady = true,
                            summary = "snapshot captured",
                            results = sharedEmptyResults,
                        ),
                        message = "snapshot captured",
                    ),
                ),
            ),
        )

        val result = useCases.runMemorySearch()

        assertTrue(result is MemoryUseCaseResult.Ok)
        val ok = result as MemoryUseCaseResult.Ok
        assertTrue(ok.value.snapshotReady)
        assertTrue(ok.value.results === sharedEmptyResults)
        assertEquals("snapshot captured", ok.value.summary)
        assertEquals("snapshot captured", ok.message)
    }

    @Test
    fun runMemorySearch_error_returnsError() = runBlocking {
        val useCases = MemoryUseCases(
            FakeMemoryGateway(
                nextResult = MemoryGatewayResult.Error("search failed"),
            ),
        )

        val result = useCases.runMemorySearch()

        assertTrue(result is MemoryUseCaseResult.Error)
        assertEquals("search failed", (result as MemoryUseCaseResult.Error).message)
    }

    @Test
    fun gatewaySearchProgress_snapshotOnly_countsAsProgressEvenWhenResultsReferenceUnchanged() {
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
    fun gatewaySearchProgress_messageOnly_doesNotCountAsProgress() {
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
        assertTrue(!didMemorySearchMakeProgress(before, after))
    }

    private class FakeMemoryGateway(
        private val nextResult: MemoryGatewayResult,
    ) : MemoryGateway {
        override fun currentState(): MemoryGatewayState =
            (nextResult as? MemoryGatewayResult.Ok)?.state
                ?: MemoryGatewayState(
                    page = null,
                    search = emptySearch(),
                    message = "",
                )

        override suspend fun openMemoryPage(remoteAddr: ULong): MemoryGatewayResult = nextResult

        override suspend fun selectMemoryAddress(remoteAddr: ULong): MemoryGatewayResult = nextResult

        override suspend fun writeHexAtFocus(): MemoryGatewayResult = nextResult

        override suspend fun writeAsciiAtFocus(): MemoryGatewayResult = nextResult

        override suspend fun runMemorySearch(): MemoryGatewayResult = nextResult

        override suspend fun refineMemorySearch(): MemoryGatewayResult = nextResult
    }

    private fun emptySearch(): MemorySearchGatewayState =
        MemorySearchGatewayState(
            snapshotReady = false,
            summary = "",
            results = emptyList(),
        )

    private fun page(focusAddress: ULong): MemoryPage =
        MemoryPage(
            focusAddress = focusAddress,
            pageStart = focusAddress,
            pageSize = 256u,
            bytes = ByteArray(0),
            rows = emptyList(),
            disassembly = emptyList(),
            scalars = emptyList(),
            region = null,
        )
}
