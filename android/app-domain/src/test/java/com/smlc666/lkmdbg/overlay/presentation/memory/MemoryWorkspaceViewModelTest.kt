package com.smlc666.lkmdbg.overlay.presentation.memory

import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Test

class MemoryWorkspaceViewModelTest {
    @Test
    fun dispatch_switchTab_updatesStateFlow() = runBlocking {
        val vm = MemoryWorkspaceViewModel(MemoryWorkspaceState.initial())

        vm.dispatch(MemoryWorkspaceIntent.SwitchTab(MemoryTab.Page))

        assertEquals(MemoryTab.Page, vm.state.value.activeTab)
    }
}
