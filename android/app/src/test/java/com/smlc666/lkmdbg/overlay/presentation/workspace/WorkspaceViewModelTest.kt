package com.smlc666.lkmdbg.overlay.presentation.workspace

import com.smlc666.lkmdbg.data.WorkspaceSection
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Test

class WorkspaceViewModelTest {
    @Test
    fun dispatch_setSection_updatesStateFlow() = runBlocking {
        val vm = WorkspaceViewModel(WorkspaceUiState.initial())

        vm.dispatch(WorkspaceIntent.SelectSection(WorkspaceSection.Events))

        assertEquals(WorkspaceSection.Events, vm.state.value.section)
    }
}
