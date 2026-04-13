package com.smlc666.lkmdbg.overlay.ui.screens

import com.smlc666.lkmdbg.data.WorkspaceSection
import com.smlc666.lkmdbg.overlay.presentation.workspace.WorkspaceIntent

data class WorkspaceDispatchAdapters(
    val openEventThread: (Int) -> Unit,
)

fun workspaceDispatchAdapters(dispatch: (WorkspaceIntent) -> Unit): WorkspaceDispatchAdapters = WorkspaceDispatchAdapters(
    openEventThread = { tid ->
        dispatch(WorkspaceIntent.SelectSection(WorkspaceSection.Threads))
        dispatch(WorkspaceIntent.SelectThread(tid))
    },
)
