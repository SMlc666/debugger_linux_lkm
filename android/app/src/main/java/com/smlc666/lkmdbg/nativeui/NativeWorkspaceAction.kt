package com.smlc666.lkmdbg.nativeui

import com.smlc666.lkmdbg.data.WorkspaceSection

internal sealed class NativeWorkspaceAction {
    data class SelectSection(val section: WorkspaceSection) : NativeWorkspaceAction()
    data class AttachProcess(val pid: Int) : NativeWorkspaceAction()
    data class OpenMemoryResult(val address: ULong) : NativeWorkspaceAction()
    data class FocusMemoryRow(val address: ULong) : NativeWorkspaceAction()
    data object RefreshProcesses : NativeWorkspaceAction()
    data object CycleProcessFilter : NativeWorkspaceAction()
    data object SearchMemory : NativeWorkspaceAction()
    data object RefineMemorySearch : NativeWorkspaceAction()
    data object CycleMemoryType : NativeWorkspaceAction()
    data object CycleMemoryMode : NativeWorkspaceAction()
    data object CycleMemoryRegion : NativeWorkspaceAction()
    data object ToggleMemoryTools : NativeWorkspaceAction()
    data object ShowMemoryResults : NativeWorkspaceAction()
    data object ShowMemoryPage : NativeWorkspaceAction()
    data object PreviousMemoryPage : NativeWorkspaceAction()
    data object NextMemoryPage : NativeWorkspaceAction()
    data object PreviewSelectedPc : NativeWorkspaceAction()

    companion object {
        fun parse(raw: String): NativeWorkspaceAction? {
            if (raw.isBlank())
                return null
            return when {
                raw.startsWith("section:") -> raw.substringAfter(':').toIntOrNull()?.let {
                    SelectSection(WorkspaceSection.fromOrdinal(it))
                }

                raw.startsWith("process:attach:") -> raw.substringAfterLast(':').toIntOrNull()?.let {
                    AttachProcess(it)
                }

                raw == "process:refresh" -> RefreshProcesses
                raw == "process:filter_next" -> CycleProcessFilter
                raw == "memory:search" -> SearchMemory
                raw == "memory:refine" -> RefineMemorySearch
                raw == "memory:type_next" -> CycleMemoryType
                raw == "memory:mode_next" -> CycleMemoryMode
                raw == "memory:region_next" -> CycleMemoryRegion
                raw == "memory:toggle_tools" -> ToggleMemoryTools
                raw == "memory:show_results" -> ShowMemoryResults
                raw == "memory:show_page" -> ShowMemoryPage
                raw == "memory:prev_page" -> PreviousMemoryPage
                raw == "memory:next_page" -> NextMemoryPage
                raw == "memory:preview_pc" -> PreviewSelectedPc
                raw.startsWith("memory:open:") -> raw.substringAfterLast(':').toULongOrNull()?.let {
                    OpenMemoryResult(it)
                }

                raw.startsWith("memory:focus:") -> raw.substringAfterLast(':').toULongOrNull()?.let {
                    FocusMemoryRow(it)
                }

                else -> null
            }
        }
    }
}
