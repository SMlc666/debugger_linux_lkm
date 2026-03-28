package com.smlc666.lkmdbg.ui

import androidx.annotation.StringRes
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Bolt
import androidx.compose.material.icons.rounded.Dns
import androidx.compose.material.icons.rounded.Memory
import androidx.compose.material.icons.rounded.Radar
import androidx.compose.ui.graphics.vector.ImageVector
import com.smlc666.lkmdbg.R

internal enum class WorkspaceTab(
    @StringRes val titleRes: Int,
    val icon: ImageVector,
) {
    Session(R.string.workspace_session, Icons.Rounded.Dns),
    Memory(R.string.workspace_memory, Icons.Rounded.Memory),
    Threads(R.string.workspace_threads, Icons.Rounded.Bolt),
    Events(R.string.workspace_events, Icons.Rounded.Radar),
}
