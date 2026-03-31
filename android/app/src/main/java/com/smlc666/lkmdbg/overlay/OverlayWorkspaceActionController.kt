package com.smlc666.lkmdbg.overlay

import com.smlc666.lkmdbg.data.SessionBridgeRepository
import com.smlc666.lkmdbg.data.WorkspaceSection
import com.smlc666.lkmdbg.nativeui.MemoryWorkspaceViewMode
import com.smlc666.lkmdbg.nativeui.NativeWorkspaceAction

internal class OverlayWorkspaceActionController(
    private val repository: SessionBridgeRepository,
    private val launchAction: (suspend () -> Unit) -> Unit,
    private val onSectionSelected: (WorkspaceSection) -> Unit,
    private val onMemoryToolsToggled: () -> Unit,
    private val onMemoryViewModeChanged: (MemoryWorkspaceViewMode) -> Unit,
    private val onProcessPickerDismissed: () -> Unit,
) {
    fun dispatch(action: NativeWorkspaceAction) {
        when (action) {
            is NativeWorkspaceAction.SelectSection -> {
                repository.updateWorkspaceSection(action.section)
                onSectionSelected(action.section)
            }

            NativeWorkspaceAction.RefreshProcesses -> launchAction { repository.refreshProcesses() }
            NativeWorkspaceAction.CycleProcessFilter -> repository.cycleProcessFilter()
            is NativeWorkspaceAction.AttachProcess -> launchAction {
                if (repository.attachProcess(action.pid)) {
                    repository.updateWorkspaceSection(WorkspaceSection.Memory)
                    onProcessPickerDismissed()
                    onSectionSelected(WorkspaceSection.Memory)
                    onMemoryViewModeChanged(MemoryWorkspaceViewMode.Page)
                }
            }

            NativeWorkspaceAction.SearchMemory -> launchAction {
                repository.runMemorySearch()
                onMemoryViewModeChanged(MemoryWorkspaceViewMode.Results)
            }

            NativeWorkspaceAction.RefineMemorySearch -> launchAction {
                repository.refineMemorySearch()
                onMemoryViewModeChanged(MemoryWorkspaceViewMode.Results)
            }

            NativeWorkspaceAction.CycleMemoryType -> repository.cycleMemorySearchValueType()
            NativeWorkspaceAction.CycleMemoryMode -> repository.cycleMemorySearchRefineMode()
            NativeWorkspaceAction.CycleMemoryRegion -> repository.cycleMemoryRegionPreset()
            NativeWorkspaceAction.ToggleMemoryTools -> onMemoryToolsToggled()
            NativeWorkspaceAction.ShowMemoryResults -> onMemoryViewModeChanged(MemoryWorkspaceViewMode.Results)
            NativeWorkspaceAction.ShowMemoryPage -> onMemoryViewModeChanged(MemoryWorkspaceViewMode.Page)
            NativeWorkspaceAction.PreviousMemoryPage -> launchAction {
                repository.stepMemoryPage(-1)
                onMemoryViewModeChanged(MemoryWorkspaceViewMode.Page)
            }

            NativeWorkspaceAction.NextMemoryPage -> launchAction {
                repository.stepMemoryPage(1)
                onMemoryViewModeChanged(MemoryWorkspaceViewMode.Page)
            }

            NativeWorkspaceAction.PreviewSelectedPc -> launchAction {
                repository.previewSelectedPc()
                onMemoryViewModeChanged(MemoryWorkspaceViewMode.Page)
            }

            is NativeWorkspaceAction.OpenMemoryResult -> launchAction {
                repository.selectMemoryAddress(action.address)
                onMemoryViewModeChanged(MemoryWorkspaceViewMode.Page)
            }

            is NativeWorkspaceAction.FocusMemoryRow -> launchAction {
                repository.selectMemoryAddress(action.address)
                onMemoryViewModeChanged(MemoryWorkspaceViewMode.Page)
            }
        }
    }
}
