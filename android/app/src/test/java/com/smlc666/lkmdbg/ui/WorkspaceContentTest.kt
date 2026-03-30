package com.smlc666.lkmdbg.ui

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.test.assertDoesNotExist
import androidx.compose.ui.test.assertExists
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.performClick
import com.smlc666.lkmdbg.data.MemoryRegionPreset
import com.smlc666.lkmdbg.data.MemorySearchRefineMode
import com.smlc666.lkmdbg.data.MemorySearchValueType
import com.smlc666.lkmdbg.data.ProcessFilter
import com.smlc666.lkmdbg.data.ResolvedProcessRecord
import com.smlc666.lkmdbg.data.SessionBridgeState
import com.smlc666.lkmdbg.shared.BridgeStatusSnapshot
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner

@RunWith(RobolectricTestRunner::class)
class WorkspaceContentTest {
    @get:Rule
    val composeRule = createComposeRule()

    @Test
    fun categoryClickSwitchesFromSessionToProcesses() {
        val state = sampleSessionState()
        composeRule.setContent {
            var selectedTab by mutableStateOf(WorkspaceTab.Session)
            var processFilter by mutableStateOf(ProcessFilter.All)
            WorkspaceContent(
                dashboardState = sampleDashboard(),
                sessionState = state,
                selectedTab = selectedTab,
                onSelectTab = { selectedTab = it },
                processFilter = processFilter,
                onProcessFilterChanged = { processFilter = it },
                actions = fakeWorkspaceActions(),
                showStatusStrip = false,
            )
        }

        composeRule.onNodeWithTag("workspace-screen-session").assertExists()
        composeRule.onNodeWithTag("workspace-screen-processes").assertDoesNotExist()
        composeRule.onNodeWithTag("workspace-tab-Processes").performClick()
        composeRule.waitForIdle()
        composeRule.onNodeWithTag("workspace-screen-processes").assertExists()
        composeRule.onNodeWithTag("workspace-screen-session").assertDoesNotExist()
    }
}

private fun sampleDashboard(): DashboardState =
    DashboardState(
        chips = listOf("root bridge"),
        scanSummary = "scan ok",
        memoryRows = emptyList(),
    )

internal fun sampleSessionState(): SessionBridgeState =
    SessionBridgeState(
        agentPath = "/data/local/tmp/lkmdbg-agent",
        lastMessage = "ok",
        snapshot = BridgeStatusSnapshot(
            status = 0,
            connected = true,
            targetPid = 1234,
            targetTid = 1234,
            sessionOpen = true,
            agentPid = 4321,
            ownerPid = 4321,
            hookActive = 1,
            eventQueueDepth = 0u,
            sessionId = 1u,
            transport = "stdio-pipe",
            message = "ok",
        ),
        processes = listOf(
            ResolvedProcessRecord(
                pid = 100,
                uid = 10123,
                comm = "android_app",
                cmdline = "com.example.game",
                processName = "com.example.game",
                displayName = "Example Game",
                packageName = "com.example.game",
                iconPackageName = null,
                isAndroidApp = true,
                isSystemApp = false,
            ),
            ResolvedProcessRecord(
                pid = 200,
                uid = 0,
                comm = "toybox",
                cmdline = "/system/bin/sh",
                processName = "/system/bin/sh",
                displayName = "toybox",
                packageName = null,
                iconPackageName = null,
                isAndroidApp = false,
                isSystemApp = false,
            ),
        ),
    )

private fun fakeWorkspaceActions(): WorkspaceActions =
    WorkspaceActions(
        onConnect = {},
        onOpenSession = {},
        onRefreshStatus = {},
        onAttachTarget = {},
        onRefreshProcesses = {},
        onRefreshThreads = {},
        onRefreshEvents = {},
        onRefreshImages = {},
        onRefreshVmas = {},
        onSearchMemory = {},
        onRefineMemory = {},
        onPreviewSelectedPc = {},
        onJumpMemoryAddress = {},
        onStepMemoryPage = { _: Int -> },
        onLoadSelectionIntoHexSearch = {},
        onLoadSelectionIntoAsciiSearch = {},
        onLoadSelectionIntoEditors = {},
        onWriteHexAtFocus = {},
        onWriteAsciiAtFocus = {},
        onAssembleArm64ToEditors = {},
        onAssembleArm64AndWrite = {},
        onSelectMemoryAddress = { _: ULong -> },
        onAttachProcess = { _: Int -> },
        onSelectThread = { _: Int -> },
        onTargetPidChanged = { _: String -> },
        onMemoryAddressChanged = { _: String -> },
        onMemorySelectionSizeChanged = { _: Int -> },
        onMemoryWriteHexChanged = { _: String -> },
        onMemoryWriteAsciiChanged = { _: String -> },
        onMemoryWriteAsmChanged = { _: String -> },
        onProcessFilterChanged = { _: ProcessFilter -> },
        onMemorySearchQueryChanged = { _: String -> },
        onMemorySearchValueTypeChanged = { _: MemorySearchValueType -> },
        onMemorySearchRefineModeChanged = { _: MemorySearchRefineMode -> },
        onMemoryRegionPresetChanged = { _: MemoryRegionPreset -> },
    )
