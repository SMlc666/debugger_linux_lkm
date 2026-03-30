package com.smlc666.lkmdbg.ui.screens

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.performClick
import com.smlc666.lkmdbg.data.ProcessFilter
import com.smlc666.lkmdbg.ui.sampleSessionState
import org.junit.Assert.assertEquals
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
            var processFilter by mutableStateOf(ProcessFilter.All)
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

        assertNodeCount("Example Game", 1)
        assertNodeCount("toybox", 1)

        composeRule.onNodeWithTag("process-filter-AndroidApps").performClick()
        assertNodeCount("Example Game", 1)
        assertNodeCount("toybox", 0)

        composeRule.onNodeWithTag("process-filter-CommandLine").performClick()
        assertNodeCount("toybox", 1)
        assertNodeCount("Example Game", 0)
    }

    private fun assertNodeCount(text: String, expected: Int) {
        val count = composeRule.onAllNodesWithText(text).fetchSemanticsNodes().size
        assertEquals("unexpected node count for '$text'", expected, count)
    }
}
