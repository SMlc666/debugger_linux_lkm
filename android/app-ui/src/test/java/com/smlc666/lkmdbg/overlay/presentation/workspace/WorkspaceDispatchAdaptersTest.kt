package com.smlc666.lkmdbg.overlay.presentation.workspace

import com.smlc666.lkmdbg.data.WorkspaceSection
import com.smlc666.lkmdbg.overlay.ui.screens.workspaceDispatchAdapters
import org.junit.Assert.assertEquals
import org.junit.Test

class WorkspaceDispatchAdaptersTest {
    @Test
    fun map_openEventThread_emitsSelectThreadAndSectionIntent() {
        val intents = mutableListOf<WorkspaceIntent>()
        val adapters = workspaceDispatchAdapters(intents::add)

        adapters.openEventThread(1234)

        assertEquals(
            listOf(
                WorkspaceIntent.SelectSection(WorkspaceSection.Threads),
                WorkspaceIntent.SelectThread(1234),
            ),
            intents,
        )
    }
}
