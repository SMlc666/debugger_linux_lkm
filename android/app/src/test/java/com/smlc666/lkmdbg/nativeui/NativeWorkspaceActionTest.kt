package com.smlc666.lkmdbg.nativeui

import com.smlc666.lkmdbg.data.WorkspaceSection
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class NativeWorkspaceActionTest {
    @Test
    fun parsesSectionSelection() {
        val action = NativeWorkspaceAction.parse("section:2")
        assertEquals(
            NativeWorkspaceAction.SelectSection(WorkspaceSection.Memory),
            action,
        )
    }

    @Test
    fun parsesProcessAttach() {
        val action = NativeWorkspaceAction.parse("process:attach:1337")
        assertEquals(
            NativeWorkspaceAction.AttachProcess(1337),
            action,
        )
    }

    @Test
    fun parsesMemoryAddresses() {
        val open = NativeWorkspaceAction.parse("memory:open:4096")
        val focus = NativeWorkspaceAction.parse("memory:focus:8192")
        assertEquals(NativeWorkspaceAction.OpenMemoryResult(4096u), open)
        assertEquals(NativeWorkspaceAction.FocusMemoryRow(8192u), focus)
    }

    @Test
    fun ignoresUnknownAction() {
        assertTrue(NativeWorkspaceAction.parse("nope") == null)
    }

    @Test
    fun parsesMemoryUiActions() {
        assertEquals(
            NativeWorkspaceAction.ToggleMemoryTools,
            NativeWorkspaceAction.parse("memory:toggle_tools"),
        )
        assertEquals(
            NativeWorkspaceAction.ShowMemoryResults,
            NativeWorkspaceAction.parse("memory:show_results"),
        )
        assertEquals(
            NativeWorkspaceAction.ShowMemoryPage,
            NativeWorkspaceAction.parse("memory:show_page"),
        )
    }
}
