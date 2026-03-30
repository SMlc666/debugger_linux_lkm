package com.smlc666.lkmdbg.ui.screens

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.performClick
import com.smlc666.lkmdbg.data.ProcessFilter
import com.smlc666.lkmdbg.ui.sampleSessionState
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner

@RunWith(RobolectricTestRunner::class)
class SessionScreenTest {
    @get:Rule
    val composeRule = createComposeRule()

    @Test
    fun quickAttachFiltersChangeVisibleRows() {
        val state = sampleSessionState()
        composeRule.setContent {
            var processFilter by remember { mutableStateOf(ProcessFilter.All) }
            SessionScreen(
                state = state,
                onConnect = {},
                onOpenSession = {},
                onRefreshStatus = {},
                onAttachTarget = {},
                onTargetPidChanged = {},
                onOpenProcessWorkspace = {},
                processFilter = processFilter,
                onRefreshProcesses = {},
                onProcessFilterChanged = { processFilter = it },
                onAttachProcess = {},
            )
        }

        composeRule.onNodeWithTag("quick-attach-list-All").assertExists()
        composeRule.onNodeWithTag("quick-attach-row-100-All").assertExists()
        composeRule.onNodeWithTag("quick-attach-row-200-All").assertExists()

        composeRule.onNodeWithTag("process-filter-AndroidApps").performClick()
        composeRule.waitForIdle()
        composeRule.onNodeWithTag("quick-attach-list-AndroidApps").assertExists()
        composeRule.onNodeWithTag("quick-attach-row-100-AndroidApps").assertExists()
        composeRule.onNodeWithTag("quick-attach-row-200-AndroidApps").assertDoesNotExist()

        composeRule.onNodeWithTag("process-filter-CommandLine").performClick()
        composeRule.waitForIdle()
        composeRule.onNodeWithTag("quick-attach-list-CommandLine").assertExists()
        composeRule.onNodeWithTag("quick-attach-row-200-CommandLine").assertExists()
        composeRule.onNodeWithTag("quick-attach-row-100-CommandLine").assertDoesNotExist()
    }
}
