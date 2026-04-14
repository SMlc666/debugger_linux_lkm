package com.smlc666.lkmdbg.platform.overlay

import com.smlc666.lkmdbg.data.SessionBridgeRepository
import com.smlc666.lkmdbg.data.SessionBridgeState
import com.smlc666.lkmdbg.data.WorkspaceSection
import com.smlc666.lkmdbg.overlay.OverlayProcessPickerController
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.flow.StateFlow

class OverlayHostController internal constructor(
    private val repository: HostRepository,
    private val processPickerController: HostProcessPickerController,
    private val scope: CoroutineScope,
    private val eventAutoPollJobFactory: EventAutoPollJobFactory = DefaultEventAutoPollJobFactory,
) {
    internal interface HostRepository {
        val state: StateFlow<SessionBridgeState>

        fun updateWorkspaceSection(section: WorkspaceSection)
        fun updateEventsAutoPollEnabled(enabled: Boolean)
        fun updateMemoryToolsOpen(open: Boolean)
        fun updateMemoryViewMode(mode: Int)
        suspend fun refreshEventsBackground(timeoutMs: Int, maxEvents: Int)
    }

    internal interface HostProcessPickerController {
        fun toggle(state: SessionBridgeState)
        fun hide()
        fun isVisible(): Boolean
        fun render(state: SessionBridgeState)
    }

    internal fun interface EventAutoPollJobFactory {
        fun start(scope: CoroutineScope, repository: HostRepository): Job
    }

    private class SessionBridgeRepositoryAdapter(
        private val delegate: SessionBridgeRepository,
    ) : HostRepository {
        override val state: StateFlow<SessionBridgeState>
            get() = delegate.state

        override fun updateWorkspaceSection(section: WorkspaceSection) {
            delegate.updateWorkspaceSection(section)
        }

        override fun updateEventsAutoPollEnabled(enabled: Boolean) {
            delegate.updateEventsAutoPollEnabled(enabled)
        }

        override fun updateMemoryToolsOpen(open: Boolean) {
            delegate.updateMemoryToolsOpen(open)
        }

        override fun updateMemoryViewMode(mode: Int) {
            delegate.updateMemoryViewMode(mode)
        }

        override suspend fun refreshEventsBackground(timeoutMs: Int, maxEvents: Int) {
            delegate.refreshEventsBackground(timeoutMs = timeoutMs, maxEvents = maxEvents)
        }
    }

    private class OverlayProcessPickerControllerAdapter(
        private val delegate: OverlayProcessPickerController,
    ) : HostProcessPickerController {
        override fun toggle(state: SessionBridgeState) {
            delegate.toggle(state)
        }

        override fun hide() {
            delegate.hide()
        }

        override fun isVisible(): Boolean {
            return delegate.isVisible()
        }

        override fun render(state: SessionBridgeState) {
            delegate.render(state)
        }
    }

    private object DefaultEventAutoPollJobFactory : EventAutoPollJobFactory {
        override fun start(scope: CoroutineScope, repository: HostRepository): Job {
            return scope.launch {
                while (isActive) {
                    repository.refreshEventsBackground(timeoutMs = 100, maxEvents = 16)
                    delay(400)
                }
            }
        }
    }

    constructor(
        repository: SessionBridgeRepository,
        processPickerController: OverlayProcessPickerController,
        scope: CoroutineScope,
    ) : this(
        repository = SessionBridgeRepositoryAdapter(repository),
        processPickerController = OverlayProcessPickerControllerAdapter(processPickerController),
        scope = scope,
        eventAutoPollJobFactory = DefaultEventAutoPollJobFactory,
    )

    private var expanded: Boolean = false
    private var eventAutoPollJob: Job? = null

    fun setExpanded(expanded: Boolean) {
        this.expanded = expanded
        if (!expanded) {
            processPickerController.hide()
            stopEventAutoPoll()
        }
        renderOverlayState(repository.state.value)
    }

    fun onRepositoryStateChanged(state: SessionBridgeState) {
        renderOverlayState(state)
    }

    fun selectSection(section: WorkspaceSection) {
        repository.updateWorkspaceSection(section)
        handleSectionSelection(section)
    }

    fun toggleProcessPicker() {
        repository.updateWorkspaceSection(WorkspaceSection.Processes)
        handleSectionSelection(WorkspaceSection.Processes)
        processPickerController.toggle(repository.state.value)
    }

    fun afterProcessPickerAction() {
        if (repository.state.value.workspaceSection == WorkspaceSection.Memory) {
            setMemoryViewModePage()
            repository.updateMemoryToolsOpen(false)
        }
        renderOverlayState(repository.state.value)
    }

    fun toggleMemoryTools() {
        if (repository.state.value.workspaceSection != WorkspaceSection.Memory)
            return
        repository.updateMemoryToolsOpen(!repository.state.value.memoryToolsOpen)
        renderOverlayState(repository.state.value)
    }

    fun setMemoryViewModePage() {
        setMemoryViewMode(0)
    }

    fun setMemoryViewModeResults() {
        setMemoryViewMode(1)
    }

    fun toggleEventsAutoPollEnabled() {
        repository.updateEventsAutoPollEnabled(!repository.state.value.eventsAutoPollEnabled)
        renderOverlayState(repository.state.value)
    }

    private fun setMemoryViewMode(mode: Int) {
        if (repository.state.value.memoryViewMode == mode)
            return
        repository.updateMemoryViewMode(mode)
        renderOverlayState(repository.state.value)
    }

    private fun handleSectionSelection(section: WorkspaceSection) {
        if (section == WorkspaceSection.Events) {
            repository.updateEventsAutoPollEnabled(true)
        } else {
            repository.updateEventsAutoPollEnabled(false)
        }
        if (section != WorkspaceSection.Processes)
            processPickerController.hide()
        renderOverlayState(repository.state.value)
    }

    private fun renderOverlayState(state: SessionBridgeState) {
        if (!expanded) {
            stopEventAutoPoll()
            return
        }
        if (state.workspaceSection != WorkspaceSection.Processes)
            processPickerController.hide()

        // Rendering the process picker is expensive (rebuilds the whole list).
        // Only do it when the picker is actually visible; otherwise frequent state
        // updates (status loop / event auto-poll) can starve the Compose UI thread.
        if (processPickerController.isVisible())
            processPickerController.render(state)
        syncEventAutoPoll(state)
    }

    private fun syncEventAutoPoll(state: SessionBridgeState) {
        val shouldPoll =
            expanded &&
                state.workspaceSection == WorkspaceSection.Events &&
                state.eventsAutoPollEnabled &&
                state.snapshot.sessionOpen
        if (!shouldPoll) {
            stopEventAutoPoll()
            return
        }
        if (eventAutoPollJob?.isActive == true)
            return
        eventAutoPollJob = eventAutoPollJobFactory.start(scope, repository)
    }

    private fun stopEventAutoPoll() {
        eventAutoPollJob?.cancel()
        eventAutoPollJob = null
    }

    internal fun isEventAutoPollActiveForTests(): Boolean {
        return eventAutoPollJob?.isActive == true
    }
}
