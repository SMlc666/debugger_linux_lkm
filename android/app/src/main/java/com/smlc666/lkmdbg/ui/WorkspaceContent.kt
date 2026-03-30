package com.smlc666.lkmdbg.ui

import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.data.SessionBridgeState
import com.smlc666.lkmdbg.ui.components.StatusStrip
import com.smlc666.lkmdbg.ui.components.WorkspaceBar
import com.smlc666.lkmdbg.ui.screens.EventScreen
import com.smlc666.lkmdbg.ui.screens.MemoryScreen
import com.smlc666.lkmdbg.ui.screens.ProcessWorkspaceScreen
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
    Column(
        modifier = modifier
            .fillMaxSize()
            .padding(contentPadding),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        if (showStatusStrip) {
            StatusStrip(sessionState)
        }
        if (showWorkspaceBar) {
            WorkspaceBar(selectedTab = selectedTab, onSelect = onSelectTab)
        }
        Column(modifier = Modifier.weight(1f)) {
            WorkspaceSectionHeader(selectedTab = selectedTab)
            AnimatedContent(
                targetState = selectedTab,
                transitionSpec = {
                    fadeIn(animationSpec = tween(160)) togetherWith
                        fadeOut(animationSpec = tween(120))
                },
                label = "workspace_tab_content",
            ) { tab ->
                when (tab) {
                    WorkspaceTab.Session -> SessionScreen(
                        state = sessionState,
                        onConnect = actions.onConnect,
                        onOpenSession = actions.onOpenSession,
                        onRefreshStatus = actions.onRefreshStatus,
                        onAttachTarget = actions.onAttachTarget,
                        onTargetPidChanged = actions.onTargetPidChanged,
                        onRefreshProcesses = actions.onRefreshProcesses,
                        onProcessFilterChanged = actions.onProcessFilterChanged,
                        onAttachProcess = actions.onAttachProcess,
                    )

                    WorkspaceTab.Processes -> ProcessWorkspaceScreen(
                        state = sessionState,
                        onRefreshProcesses = actions.onRefreshProcesses,
                        onProcessFilterChanged = actions.onProcessFilterChanged,
                        onAttachProcess = actions.onAttachProcess,
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
                        onSearchRefineModeChanged = actions.onMemorySearchRefineModeChanged,
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
        }
    }
}

@Composable
private fun WorkspaceSectionHeader(selectedTab: WorkspaceTab) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(bottom = 10.dp),
        verticalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        Text(
            text = stringResource(selectedTab.titleRes),
            style = MaterialTheme.typography.headlineSmall,
            color = MaterialTheme.colorScheme.onSurface,
        )
        Text(
            text = stringResource(selectedTab.subtitleRes()),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

private fun WorkspaceTab.subtitleRes(): Int =
    when (this) {
        WorkspaceTab.Session -> R.string.workspace_session_subtitle
        WorkspaceTab.Processes -> R.string.workspace_processes_subtitle
        WorkspaceTab.Memory -> R.string.workspace_memory_subtitle
        WorkspaceTab.Threads -> R.string.workspace_threads_subtitle
        WorkspaceTab.Events -> R.string.workspace_events_subtitle
    }
