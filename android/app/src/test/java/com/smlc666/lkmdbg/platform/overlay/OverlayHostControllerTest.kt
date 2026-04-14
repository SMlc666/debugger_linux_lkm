package com.smlc666.lkmdbg.platform.overlay

import com.smlc666.lkmdbg.data.SessionBridgeState
import com.smlc666.lkmdbg.data.WorkspaceSection
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class OverlayHostControllerTest {
    private class FakeRepository(
        initialState: SessionBridgeState,
    ) : OverlayHostController.HostRepository {
        private val _state = MutableStateFlow(initialState)

        override val state: StateFlow<SessionBridgeState> = _state

        var updateWorkspaceSectionCalls: Int = 0
        var updateEventsAutoPollEnabledCalls: MutableList<Boolean> = mutableListOf()

        override fun updateWorkspaceSection(section: WorkspaceSection) {
            updateWorkspaceSectionCalls += 1
            _state.value = _state.value.copy(workspaceSection = section)
        }

        override fun updateEventsAutoPollEnabled(enabled: Boolean) {
            updateEventsAutoPollEnabledCalls.add(enabled)
            _state.value = _state.value.copy(eventsAutoPollEnabled = enabled)
        }

        override suspend fun refreshEventsBackground(timeoutMs: Int, maxEvents: Int) {
            // No-op: unit tests validate the lifecycle wiring (start/stop), not transport behavior.
        }
    }

    private class FakeProcessPickerController : OverlayHostController.HostProcessPickerController {
        var hideCalls: Int = 0
        var toggleStates: MutableList<SessionBridgeState> = mutableListOf()
        var visible: Boolean = false

        override fun toggle(state: SessionBridgeState) {
            toggleStates.add(state)
        }

        override fun hide() {
            hideCalls += 1
            visible = false
        }

        override fun isVisible(): Boolean = visible

        override fun render(state: SessionBridgeState) {
            // Not exercised by these unit tests.
        }
    }

    private class RecordingEventAutoPollJobFactory : OverlayHostController.EventAutoPollJobFactory {
        var starts: Int = 0
        val jobs: MutableList<Job> = mutableListOf()

        override fun start(
            scope: CoroutineScope,
            repository: OverlayHostController.HostRepository,
        ): Job {
            starts += 1
            return Job().also { jobs.add(it) }
        }
    }

    @Test
    fun expandedCollapsed_togglesEventAutoPoll() {
        val base = SessionBridgeState(
            agentPath = "test-agent",
            lastMessage = "idle",
        )
        val repository = FakeRepository(
            base.copy(
                workspaceSection = WorkspaceSection.Events,
                eventsAutoPollEnabled = true,
                snapshot = base.snapshot.copy(sessionOpen = true),
            ),
        )
        val picker = FakeProcessPickerController()
        val jobFactory = RecordingEventAutoPollJobFactory()
        val controller = OverlayHostController(
            repository = repository,
            processPickerController = picker,
            scope = CoroutineScope(SupervisorJob()),
            eventAutoPollJobFactory = jobFactory,
        )

        controller.setExpanded(true)
        assertTrue(controller.isEventAutoPollActiveForTests())
        assertEquals(1, jobFactory.starts)
        assertEquals(1, jobFactory.jobs.size)

        controller.onRepositoryStateChanged(repository.state.value)
        assertEquals(1, jobFactory.starts)

        controller.setExpanded(false)
        assertFalse(controller.isEventAutoPollActiveForTests())
        assertTrue(jobFactory.jobs[0].isCancelled)

        controller.setExpanded(true)
        assertTrue(controller.isEventAutoPollActiveForTests())
        assertEquals(2, jobFactory.starts)
        assertEquals(2, jobFactory.jobs.size)
    }

    @Test
    fun toggleProcessPicker_doesNotSwitchWorkspaceSection() {
        val repository = FakeRepository(
            SessionBridgeState(
                agentPath = "test-agent",
                lastMessage = "idle",
            ),
        )
        val picker = FakeProcessPickerController()
        val controller = OverlayHostController(
            repository = repository,
            processPickerController = picker,
            scope = CoroutineScope(SupervisorJob()),
        )

        controller.toggleProcessPicker()
        assertEquals(WorkspaceSection.Memory, repository.state.value.workspaceSection)
        assertEquals(0, repository.updateWorkspaceSectionCalls)
        assertEquals(emptyList<Boolean>(), repository.updateEventsAutoPollEnabledCalls)

        assertEquals(1, picker.toggleStates.size)
        assertEquals(WorkspaceSection.Memory, picker.toggleStates[0].workspaceSection)
        assertEquals(0, picker.hideCalls)
    }

    @Test
    fun toggleEventsAutoPollEnabled_togglesOncePerCall() {
        val repository = FakeRepository(
            SessionBridgeState(
                agentPath = "test-agent",
                lastMessage = "idle",
                workspaceSection = WorkspaceSection.Events,
            ),
        )
        val picker = FakeProcessPickerController()
        val controller = OverlayHostController(
            repository = repository,
            processPickerController = picker,
            scope = CoroutineScope(SupervisorJob()),
        )

        controller.toggleEventsAutoPollEnabled()
        assertEquals(listOf(true), repository.updateEventsAutoPollEnabledCalls)
        assertTrue(repository.state.value.eventsAutoPollEnabled)

        controller.toggleEventsAutoPollEnabled()
        assertEquals(listOf(true, false), repository.updateEventsAutoPollEnabledCalls)
        assertFalse(repository.state.value.eventsAutoPollEnabled)
    }
}
