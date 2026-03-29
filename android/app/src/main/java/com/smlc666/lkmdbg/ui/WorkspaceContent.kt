package com.smlc666.lkmdbg.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.smlc666.lkmdbg.data.SessionBridgeState
import com.smlc666.lkmdbg.ui.components.StatusStrip
import com.smlc666.lkmdbg.ui.components.WorkspaceBar
import com.smlc666.lkmdbg.ui.screens.EventScreen
import com.smlc666.lkmdbg.ui.screens.MemoryScreen
import com.smlc666.lkmdbg.ui.screens.ProcessControlPanel
import com.smlc666.lkmdbg.ui.screens.ProcessRowCard
import com.smlc666.lkmdbg.ui.screens.SessionScreen
import com.smlc666.lkmdbg.ui.screens.ThreadScreen

@Composable
internal fun WorkspaceContent(
    dashboardState: DashboardState,
    sessionState: SessionBridgeState,
    selectedTab: WorkspaceTab,
    onSelectTab: (WorkspaceTab) -> Unit,
    actions: WorkspaceActions,
    modifier: Modifier = Modifier,
    contentPadding: PaddingValues = PaddingValues(horizontal = 14.dp, vertical = 12.dp),
    showStatusStrip: Boolean = true,
    showWorkspaceBar: Boolean = true,
) {
    val filteredProcesses = remember(sessionState.processes, sessionState.processFilter) {
        sessionState.processes.filter { sessionState.processFilter.matches(it) }
    }

    LazyColumn(
        modifier = modifier.fillMaxSize(),
        contentPadding = contentPadding,
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        if (showStatusStrip) {
            item { StatusStrip(sessionState) }
        }
        if (showWorkspaceBar) {
            item {
                WorkspaceBar(selectedTab = selectedTab, onSelect = onSelectTab)
            }
        }
        item {
            when (selectedTab) {
                WorkspaceTab.Session -> SessionScreen(
                    state = sessionState,
                    onConnect = actions.onConnect,
                    onOpenSession = actions.onOpenSession,
                    onRefreshStatus = actions.onRefreshStatus,
                    onAttachTarget = actions.onAttachTarget,
                    onTargetPidChanged = actions.onTargetPidChanged,
                )

                WorkspaceTab.Processes -> ProcessControlPanel(
                    state = sessionState,
                    onRefreshProcesses = actions.onRefreshProcesses,
                    onProcessFilterChanged = actions.onProcessFilterChanged,
                )

                WorkspaceTab.Memory -> MemoryScreen(
                    dashboardState = dashboardState,
                    state = sessionState,
                    onRefreshImages = actions.onRefreshImages,
                    onRefreshVmas = actions.onRefreshVmas,
                    onSearchMemory = actions.onSearchMemory,
                    onRefineMemory = actions.onRefineMemory,
                    onPreviewSelectedPc = actions.onPreviewSelectedPc,
                    onJumpMemoryAddress = actions.onJumpMemoryAddress,
                    onStepMemoryPage = actions.onStepMemoryPage,
                    onLoadSelectionIntoHexSearch = actions.onLoadSelectionIntoHexSearch,
                    onLoadSelectionIntoAsciiSearch = actions.onLoadSelectionIntoAsciiSearch,
                    onLoadSelectionIntoEditors = actions.onLoadSelectionIntoEditors,
                    onWriteHexAtFocus = actions.onWriteHexAtFocus,
                    onWriteAsciiAtFocus = actions.onWriteAsciiAtFocus,
                    onAssembleArm64ToEditors = actions.onAssembleArm64ToEditors,
                    onAssembleArm64AndWrite = actions.onAssembleArm64AndWrite,
                    onSelectMemoryAddress = actions.onSelectMemoryAddress,
                    onMemoryAddressChanged = actions.onMemoryAddressChanged,
                    onSelectionSizeChanged = actions.onMemorySelectionSizeChanged,
                    onWriteHexChanged = actions.onMemoryWriteHexChanged,
                    onWriteAsciiChanged = actions.onMemoryWriteAsciiChanged,
                    onWriteAsmChanged = actions.onMemoryWriteAsmChanged,
                    onSearchQueryChanged = actions.onMemorySearchQueryChanged,
                    onSearchValueTypeChanged = actions.onMemorySearchValueTypeChanged,
                    onRegionPresetChanged = actions.onMemoryRegionPresetChanged,
                )

                WorkspaceTab.Threads -> ThreadScreen(
                    state = sessionState,
                    onRefreshThreads = actions.onRefreshThreads,
                    onSelectThread = actions.onSelectThread,
                )

                WorkspaceTab.Events -> EventScreen(
                    state = sessionState,
                    onRefreshEvents = actions.onRefreshEvents,
                )
            }
        }
        if (selectedTab == WorkspaceTab.Processes) {
            if (filteredProcesses.isEmpty()) {
                item {
                    Text(
                        text = sessionState.lastMessage,
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            } else {
                items(filteredProcesses, key = { "${it.pid}:${it.processName}" }) { process ->
                    ProcessRowCard(
                        process = process,
                        onAttach = { actions.onAttachProcess(process.pid) },
                    )
                }
            }
        }
    }
}
