package com.smlc666.lkmdbg.overlay.presentation.workspace

import com.smlc666.lkmdbg.data.WorkspaceSection
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class WorkspaceViewModelTest {
    @Test
    fun dispatch_setSection_updatesStateFlow() = runBlocking {
        val vm = WorkspaceViewModel(WorkspaceUiState.initial())

        vm.dispatch(WorkspaceIntent.SelectSection(WorkspaceSection.Events))

        assertEquals(WorkspaceSection.Events, vm.state.value.section)
    }

    @Test
    fun dispatch_repeatedSelectSection_keepsLastSectionState() = runBlocking {
        val vm = WorkspaceViewModel(WorkspaceUiState.initial())

        vm.dispatch(WorkspaceIntent.SelectSection(WorkspaceSection.Threads))
        vm.dispatch(WorkspaceIntent.SelectSection(WorkspaceSection.Events))
        vm.dispatch(WorkspaceIntent.SelectSection(WorkspaceSection.Memory))

        assertEquals(WorkspaceSection.Memory, vm.state.value.section)
    }

    @Test
    fun dispatch_concurrentSelectSection_keepsStateValidAndStable() = runBlocking {
        val vm = WorkspaceViewModel(WorkspaceUiState.initial())
        val sections = listOf(
            WorkspaceSection.Session,
            WorkspaceSection.Processes,
            WorkspaceSection.Memory,
            WorkspaceSection.Threads,
            WorkspaceSection.Events,
        )

        repeat(50) {
            val start = CountDownLatch(1)
            val done = CountDownLatch(sections.size)
            val errors = ConcurrentLinkedQueue<Throwable>()

            val jobs = sections.map { section ->
                launch(Dispatchers.Default) {
                    try {
                        start.await()
                        vm.dispatch(WorkspaceIntent.SelectSection(section))
                    } catch (t: Throwable) {
                        errors.add(t)
                    } finally {
                        done.countDown()
                    }
                }
            }

            start.countDown()
            assertTrue("Concurrent dispatch timed out", done.await(3, TimeUnit.SECONDS))
            jobs.forEach { it.join() }

            assertTrue("No dispatch coroutine should fail", errors.isEmpty())
            assertTrue(
                "Final section must be one of dispatched sections",
                vm.state.value.section in sections,
            )
        }
    }
}
