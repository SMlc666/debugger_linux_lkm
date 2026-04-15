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

    internal constructor(
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
        processPickerController.toggle(repository.state.value)
        renderOverlayState(repository.state.value)
    }

    fun afterProcessPickerAction() {
        renderOverlayState(repository.state.value)
    }

    fun toggleEventsAutoPollEnabled() {
        repository.updateEventsAutoPollEnabled(!repository.state.value.eventsAutoPollEnabled)
        renderOverlayState(repository.state.value)
    }

    private fun handleSectionSelection(section: WorkspaceSection) {
        if (section == WorkspaceSection.Events) {
            repository.updateEventsAutoPollEnabled(true)
        } else {
            repository.updateEventsAutoPollEnabled(false)
        }
        processPickerController.hide()
        renderOverlayState(repository.state.value)
    }

    private fun renderOverlayState(state: SessionBridgeState) {
        if (!expanded) {
            stopEventAutoPoll()
            return
        }

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
