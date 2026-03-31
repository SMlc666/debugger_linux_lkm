package com.smlc666.lkmdbg.overlay

import com.smlc666.lkmdbg.data.SessionBridgeRepository
import com.smlc666.lkmdbg.data.WorkspaceSection
import com.smlc666.lkmdbg.nativeui.NativeWorkspaceAction

internal class OverlayWorkspaceActionController(
    private val repository: SessionBridgeRepository,
    private val launchAction: (suspend () -> Unit) -> Unit,
) {
    fun dispatch(action: NativeWorkspaceAction) {
        when (action) {
            is NativeWorkspaceAction.SelectSection -> repository.updateWorkspaceSection(action.section)
            NativeWorkspaceAction.RefreshProcesses -> launchAction { repository.refreshProcesses() }
            NativeWorkspaceAction.CycleProcessFilter -> repository.cycleProcessFilter()
            is NativeWorkspaceAction.AttachProcess -> launchAction {
                if (repository.attachProcess(action.pid))
                    repository.updateWorkspaceSection(WorkspaceSection.Memory)
            }

            NativeWorkspaceAction.SearchMemory -> launchAction { repository.runMemorySearch() }
            NativeWorkspaceAction.RefineMemorySearch -> launchAction { repository.refineMemorySearch() }
            NativeWorkspaceAction.CycleMemoryType -> repository.cycleMemorySearchValueType()
            NativeWorkspaceAction.CycleMemoryMode -> repository.cycleMemorySearchRefineMode()
            NativeWorkspaceAction.CycleMemoryRegion -> repository.cycleMemoryRegionPreset()
            NativeWorkspaceAction.PreviousMemoryPage -> launchAction { repository.stepMemoryPage(-1) }
            NativeWorkspaceAction.NextMemoryPage -> launchAction { repository.stepMemoryPage(1) }
            NativeWorkspaceAction.PreviewSelectedPc -> launchAction { repository.previewSelectedPc() }
            is NativeWorkspaceAction.OpenMemoryResult -> launchAction {
                repository.selectMemoryAddress(action.address)
            }

            is NativeWorkspaceAction.FocusMemoryRow -> launchAction {
                repository.selectMemoryAddress(action.address)
            }
        }
    }
}
