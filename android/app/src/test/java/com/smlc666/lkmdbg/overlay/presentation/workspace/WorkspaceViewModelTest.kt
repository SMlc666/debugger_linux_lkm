package com.smlc666.lkmdbg.overlay.presentation.workspace

import com.smlc666.lkmdbg.data.WorkspaceSection
import com.smlc666.lkmdbg.domain.event.EventUseCases
import com.smlc666.lkmdbg.domain.gateway.EventEntry
import com.smlc666.lkmdbg.domain.gateway.EventGateway
import com.smlc666.lkmdbg.domain.gateway.EventGatewayResult
import com.smlc666.lkmdbg.domain.gateway.EventGatewayState
import com.smlc666.lkmdbg.domain.gateway.ProcessRecord
import com.smlc666.lkmdbg.domain.gateway.ThreadGateway
import com.smlc666.lkmdbg.domain.gateway.ThreadGatewayResult
import com.smlc666.lkmdbg.domain.gateway.ThreadGatewayState
import com.smlc666.lkmdbg.domain.gateway.ThreadRecord
import com.smlc666.lkmdbg.domain.thread.ThreadUseCases
import com.smlc666.lkmdbg.shared.BridgeEventRecord
import com.smlc666.lkmdbg.shared.BridgeThreadRegistersReply
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test

class WorkspaceViewModelTest {
    @Test
    fun dispatch_setSection_updatesStateFlow() = runBlocking {
        val vm = newViewModel()

        vm.dispatch(WorkspaceIntent.SelectSection(WorkspaceSection.Events))

        assertEquals(WorkspaceSection.Events, vm.state.value.section)
    }

    @Test
    fun dispatch_repeatedSelectSection_keepsLastSectionState() = runBlocking {
        val vm = newViewModel()

        vm.dispatch(WorkspaceIntent.SelectSection(WorkspaceSection.Threads))
        vm.dispatch(WorkspaceIntent.SelectSection(WorkspaceSection.Events))
        vm.dispatch(WorkspaceIntent.SelectSection(WorkspaceSection.Memory))

        assertEquals(WorkspaceSection.Memory, vm.state.value.section)
    }

    @Test
    fun dispatch_selectThread_callsUseCaseAndUpdatesThreadSlice() = runBlocking {
        val gateway = FakeThreadGateway()
        val vm = newViewModel(threadGateway = gateway)

        vm.dispatch(WorkspaceIntent.SelectThread(77))

        val next = withTimeout(1_000) { vm.state.first { it.threads.selectedTid == 77 } }

        assertEquals(1, gateway.selectThreadCalls)
        assertEquals(77, gateway.lastSelectedTid)
        assertEquals(77, next.threads.selectedTid)
        assertNotNull(next.threads.selectedRegisters)
        assertTrue(next.threads.items.any { it.tid == 77 })
    }

    @Test
    fun dispatch_togglePinnedEvent_callsUseCaseAndUpdatesEventSlice() = runBlocking {
        val gateway = FakeEventGateway()
        val vm = newViewModel(eventGateway = gateway)

        vm.dispatch(WorkspaceIntent.TogglePinnedEvent(2uL))

        val next = withTimeout(1_000) { vm.state.first { 2uL in it.events.pinnedEventSeqs } }

        assertEquals(1, gateway.togglePinnedCalls)
        assertEquals(2uL, gateway.lastTogglePinnedSeq)
        assertTrue(next.events.items.isNotEmpty())
        assertEquals(2uL, next.events.items.first().record.seq)
    }

    @Test
    fun dispatch_selectThread_error_updatesMessage_andPreservesOtherSlices() = runBlocking {
        val gate = CompletableDeferred<Unit>()
        val gateway = BlockingThreadGateway(
            gate = gate,
            result = ThreadGatewayResult.Error("boom"),
        )
        val initial = WorkspaceUiState.initial().copy(
            section = WorkspaceSection.Threads,
            session = SessionUiState(isConnected = true, isSessionOpen = true, message = "sess"),
            processes = ProcessUiState(
                items = listOf(ProcessRecord(pid = 123, displayName = "p0")),
                message = "proc",
            ),
            threads = ThreadUiState(
                items = listOf(ThreadRecord(tid = 7, tgid = 1, comm = "t0")),
                selectedTid = 7,
                selectedRegisters = null,
                message = "old",
            ),
            events = EventUiState(
                items = listOf(eventEntry(9uL)),
                pinnedEventSeqs = setOf(9uL),
                message = "evt",
            ),
        )
        val vm = newViewModel(
            initialState = initial,
            threadGateway = gateway,
        )

        vm.dispatch(WorkspaceIntent.SelectThread(77))
        vm.dispatch(WorkspaceIntent.SelectSection(WorkspaceSection.Events))
        gate.complete(Unit)

        val next = withTimeout(1_000) { vm.state.first { it.threads.message == "boom" } }

        assertEquals(WorkspaceSection.Events, next.section)
        assertEquals(initial.session, next.session)
        assertEquals(initial.processes, next.processes)
        assertEquals(initial.events, next.events)
        assertEquals(initial.threads.items, next.threads.items)
        assertEquals(initial.threads.selectedTid, next.threads.selectedTid)
        assertEquals(initial.threads.selectedRegisters, next.threads.selectedRegisters)
    }

    @Test
    fun dispatch_togglePinnedEvent_error_updatesMessage_andPreservesOtherSlices() = runBlocking {
        val gate = CompletableDeferred<Unit>()
        val gateway = BlockingEventGateway(
            gate = gate,
            result = EventGatewayResult.Error("boom"),
        )
        val initialEvents = EventUiState(
            items = listOf(eventEntry(1uL), eventEntry(2uL)),
            pinnedEventSeqs = setOf(2uL),
            message = "old",
        )
        val initial = WorkspaceUiState.initial().copy(
            section = WorkspaceSection.Events,
            session = SessionUiState(isConnected = true, isSessionOpen = true, message = "sess"),
            processes = ProcessUiState(
                items = listOf(ProcessRecord(pid = 123, displayName = "p0")),
                message = "proc",
            ),
            threads = ThreadUiState(
                items = listOf(ThreadRecord(tid = 7, tgid = 1, comm = "t0")),
                selectedTid = 7,
                selectedRegisters = null,
                message = "thr",
            ),
            events = initialEvents,
        )
        val vm = newViewModel(
            initialState = initial,
            eventGateway = gateway,
        )

        vm.dispatch(WorkspaceIntent.TogglePinnedEvent(2uL))
        vm.dispatch(WorkspaceIntent.SelectSection(WorkspaceSection.Memory))
        gate.complete(Unit)

        val next = withTimeout(1_000) { vm.state.first { it.events.message == "boom" } }

        assertEquals(WorkspaceSection.Memory, next.section)
        assertEquals(initial.session, next.session)
        assertEquals(initial.processes, next.processes)
        assertEquals(initial.threads, next.threads)
        assertEquals(initialEvents.items, next.events.items)
        assertEquals(initialEvents.pinnedEventSeqs, next.events.pinnedEventSeqs)
    }

    @Test
    fun dispatch_selectThread_thenSelectSection_doesNotRollbackSectionWhenThreadReturns() = runBlocking {
        val gate = CompletableDeferred<Unit>()
        val tid = 77
        val gateway = BlockingThreadGateway(
            gate = gate,
            result = ThreadGatewayResult.Ok(
                ThreadGatewayState(
                    threads = listOf(ThreadRecord(tid = tid, tgid = 1, comm = "test")),
                    selectedTid = tid,
                    selectedRegisters = BridgeThreadRegistersReply(
                        status = 0,
                        tid = tid,
                        flags = 0u,
                        regs = ULongArray(0),
                        sp = 0uL,
                        pc = 0uL,
                        pstate = 0uL,
                        features = 0u,
                        fpsr = 0u,
                        fpcr = 0u,
                        v0Lo = 0uL,
                        v0Hi = 0uL,
                        message = "ok",
                    ),
                    message = "selected",
                ),
            ),
        )
        val vm = newViewModel(threadGateway = gateway)

        vm.dispatch(WorkspaceIntent.SelectThread(tid))
        vm.dispatch(WorkspaceIntent.SelectSection(WorkspaceSection.Events))
        gate.complete(Unit)

        val next = withTimeout(1_000) { vm.state.first { it.threads.selectedTid == tid } }

        assertEquals(WorkspaceSection.Events, next.section)
    }
}

private fun newViewModel(
    initialState: WorkspaceUiState = WorkspaceUiState.initial(),
    threadGateway: ThreadGateway = FakeThreadGateway(),
    eventGateway: EventGateway = FakeEventGateway(),
    scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.Unconfined),
): WorkspaceViewModel = WorkspaceViewModel(
    initialState = initialState,
    threadUseCases = ThreadUseCases(threadGateway),
    eventUseCases = EventUseCases(eventGateway),
    scope = scope,
)

private class FakeThreadGateway : ThreadGateway {
    var selectThreadCalls: Int = 0
        private set
    var lastSelectedTid: Int? = null
        private set

    override fun currentState(): ThreadGatewayState = ThreadGatewayState(
        threads = emptyList(),
        selectedTid = null,
        selectedRegisters = null,
        message = "",
    )

    override suspend fun refreshThreads(): ThreadGatewayResult = ThreadGatewayResult.Error("not implemented")

    override suspend fun selectThread(tid: Int): ThreadGatewayResult {
        selectThreadCalls += 1
        lastSelectedTid = tid

        val registers = BridgeThreadRegistersReply(
            status = 0,
            tid = tid,
            flags = 0u,
            regs = ULongArray(0),
            sp = 0uL,
            pc = 0uL,
            pstate = 0uL,
            features = 0u,
            fpsr = 0u,
            fpcr = 0u,
            v0Lo = 0uL,
            v0Hi = 0uL,
            message = "ok",
        )
        return ThreadGatewayResult.Ok(
            ThreadGatewayState(
                threads = listOf(ThreadRecord(tid = tid, tgid = 1, comm = "test")),
                selectedTid = tid,
                selectedRegisters = registers,
                message = "selected",
            ),
        )
    }
}

private class BlockingThreadGateway(
    private val gate: CompletableDeferred<Unit>,
    private val result: ThreadGatewayResult,
) : ThreadGateway {
    override fun currentState(): ThreadGatewayState = ThreadGatewayState(
        threads = emptyList(),
        selectedTid = null,
        selectedRegisters = null,
        message = "",
    )

    override suspend fun refreshThreads(): ThreadGatewayResult = ThreadGatewayResult.Error("not implemented")

    override suspend fun selectThread(tid: Int): ThreadGatewayResult {
        gate.await()
        return result
    }
}

private class FakeEventGateway : EventGateway {
    var togglePinnedCalls: Int = 0
        private set
    var lastTogglePinnedSeq: ULong? = null
        private set

    override fun currentState(): EventGatewayState = EventGatewayState(
        events = emptyList(),
        pinnedEventSeqs = emptySet(),
        message = "",
    )

    override suspend fun refreshEvents(timeoutMs: Int, maxEvents: Int): EventGatewayResult =
        EventGatewayResult.Error("not implemented")

    override suspend fun togglePinnedEvent(seq: ULong): EventGatewayResult {
        togglePinnedCalls += 1
        lastTogglePinnedSeq = seq

        val entries = listOf(eventEntry(1uL), eventEntry(2uL), eventEntry(3uL))
        return EventGatewayResult.Ok(
            EventGatewayState(
                events = entries,
                pinnedEventSeqs = setOf(seq),
                message = "toggled",
            ),
        )
    }
}

private class BlockingEventGateway(
    private val gate: CompletableDeferred<Unit>,
    private val result: EventGatewayResult,
) : EventGateway {
    override fun currentState(): EventGatewayState = EventGatewayState(
        events = emptyList(),
        pinnedEventSeqs = emptySet(),
        message = "",
    )

    override suspend fun refreshEvents(timeoutMs: Int, maxEvents: Int): EventGatewayResult =
        EventGatewayResult.Error("not implemented")

    override suspend fun togglePinnedEvent(seq: ULong): EventGatewayResult {
        gate.await()
        return result
    }
}

private fun eventEntry(seq: ULong): EventEntry = EventEntry(
    record = BridgeEventRecord(
        version = 0u,
        type = 0u,
        size = 0u,
        code = 0u,
        sessionId = 0uL,
        seq = seq,
        tgid = 0,
        tid = 0,
        flags = 0u,
        value0 = 0uL,
        value1 = 0uL,
    ),
    receivedAtMs = 0L,
)
