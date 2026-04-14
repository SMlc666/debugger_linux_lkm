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

class OverlayHostController(
    private val repository: SessionBridgeRepository,
    private val processPickerController: OverlayProcessPickerController,
    private val scope: CoroutineScope,
) {
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
        eventAutoPollJob = scope.launch {
            while (isActive) {
                repository.refreshEventsBackground(timeoutMs = 100, maxEvents = 16)
                delay(400)
            }
        }
    }

    private fun stopEventAutoPoll() {
        eventAutoPollJob?.cancel()
        eventAutoPollJob = null
    }
}
