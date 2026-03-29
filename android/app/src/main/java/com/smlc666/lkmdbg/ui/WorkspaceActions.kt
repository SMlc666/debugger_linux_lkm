package com.smlc666.lkmdbg.ui

import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import com.smlc666.lkmdbg.data.MemoryRegionPreset
import com.smlc666.lkmdbg.data.MemorySearchValueType
import com.smlc666.lkmdbg.data.ProcessFilter
import com.smlc666.lkmdbg.data.SessionBridgeRepository
import kotlinx.coroutines.launch

internal data class WorkspaceActions(
    val onConnect: () -> Unit,
    val onOpenSession: () -> Unit,
    val onRefreshStatus: () -> Unit,
    val onAttachTarget: () -> Unit,
    val onRefreshProcesses: () -> Unit,
    val onRefreshThreads: () -> Unit,
    val onRefreshEvents: () -> Unit,
    val onRefreshImages: () -> Unit,
    val onRefreshVmas: () -> Unit,
    val onSearchMemory: () -> Unit,
    val onRefineMemory: () -> Unit,
    val onPreviewSelectedPc: () -> Unit,
    val onJumpMemoryAddress: () -> Unit,
    val onStepMemoryPage: (Int) -> Unit,
    val onLoadSelectionIntoHexSearch: () -> Unit,
    val onLoadSelectionIntoAsciiSearch: () -> Unit,
    val onLoadSelectionIntoEditors: () -> Unit,
    val onWriteHexAtFocus: () -> Unit,
    val onWriteAsciiAtFocus: () -> Unit,
    val onAssembleArm64ToEditors: () -> Unit,
    val onAssembleArm64AndWrite: () -> Unit,
    val onSelectMemoryAddress: (ULong) -> Unit,
    val onAttachProcess: (Int) -> Unit,
    val onSelectThread: (Int) -> Unit,
    val onTargetPidChanged: (String) -> Unit,
    val onMemoryAddressChanged: (String) -> Unit,
    val onMemorySelectionSizeChanged: (Int) -> Unit,
    val onMemoryWriteHexChanged: (String) -> Unit,
    val onMemoryWriteAsciiChanged: (String) -> Unit,
    val onMemoryWriteAsmChanged: (String) -> Unit,
    val onProcessFilterChanged: (ProcessFilter) -> Unit,
    val onMemorySearchQueryChanged: (String) -> Unit,
    val onMemorySearchValueTypeChanged: (MemorySearchValueType) -> Unit,
    val onMemoryRegionPresetChanged: (MemoryRegionPreset) -> Unit,
)

@Composable
internal fun rememberWorkspaceActions(
    repository: SessionBridgeRepository,
    onAttachProcessSuccess: (Int) -> Unit = {},
): WorkspaceActions {
    val coroutineScope = rememberCoroutineScope()
    return remember(repository, coroutineScope, onAttachProcessSuccess) {
        WorkspaceActions(
            onConnect = { coroutineScope.launch { repository.connect() } },
            onOpenSession = { coroutineScope.launch { repository.openSession() } },
            onRefreshStatus = { coroutineScope.launch { repository.refreshStatus() } },
            onAttachTarget = { coroutineScope.launch { repository.attachTarget() } },
            onRefreshProcesses = { coroutineScope.launch { repository.refreshProcesses() } },
            onRefreshThreads = { coroutineScope.launch { repository.refreshThreads() } },
            onRefreshEvents = { coroutineScope.launch { repository.refreshEvents() } },
            onRefreshImages = { coroutineScope.launch { repository.refreshImages() } },
            onRefreshVmas = { coroutineScope.launch { repository.refreshVmas() } },
            onSearchMemory = { coroutineScope.launch { repository.runMemorySearch() } },
            onRefineMemory = { coroutineScope.launch { repository.refineMemorySearch() } },
            onPreviewSelectedPc = { coroutineScope.launch { repository.previewSelectedPc() } },
            onJumpMemoryAddress = { coroutineScope.launch { repository.jumpToMemoryAddress() } },
            onStepMemoryPage = { direction ->
                coroutineScope.launch { repository.stepMemoryPage(direction) }
            },
            onLoadSelectionIntoHexSearch = {
                coroutineScope.launch { repository.loadSelectionIntoHexSearch() }
            },
            onLoadSelectionIntoAsciiSearch = {
                coroutineScope.launch { repository.loadSelectionIntoAsciiSearch() }
            },
            onLoadSelectionIntoEditors = {
                coroutineScope.launch { repository.loadSelectionIntoEditors() }
            },
            onWriteHexAtFocus = { coroutineScope.launch { repository.writeHexAtFocus() } },
            onWriteAsciiAtFocus = { coroutineScope.launch { repository.writeAsciiAtFocus() } },
            onAssembleArm64ToEditors = {
                coroutineScope.launch { repository.assembleArm64ToEditors() }
            },
            onAssembleArm64AndWrite = {
                coroutineScope.launch { repository.assembleArm64AndWrite() }
            },
            onSelectMemoryAddress = { address ->
                coroutineScope.launch { repository.selectMemoryAddress(address) }
            },
            onAttachProcess = { processPid ->
                coroutineScope.launch {
                    if (repository.attachProcess(processPid))
                        onAttachProcessSuccess(processPid)
                }
            },
            onSelectThread = { tid ->
                coroutineScope.launch { repository.refreshThreadRegisters(tid) }
            },
            onTargetPidChanged = repository::updateTargetPidInput,
            onMemoryAddressChanged = repository::updateMemoryAddressInput,
            onMemorySelectionSizeChanged = repository::updateMemorySelectionSize,
            onMemoryWriteHexChanged = repository::updateMemoryWriteHexInput,
            onMemoryWriteAsciiChanged = repository::updateMemoryWriteAsciiInput,
            onMemoryWriteAsmChanged = repository::updateMemoryWriteAsmInput,
            onProcessFilterChanged = repository::updateProcessFilter,
            onMemorySearchQueryChanged = repository::updateMemorySearchQuery,
            onMemorySearchValueTypeChanged = repository::updateMemorySearchValueType,
            onMemoryRegionPresetChanged = repository::updateMemoryRegionPreset,
        )
    }
}
