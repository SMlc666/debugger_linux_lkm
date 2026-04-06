package com.smlc666.lkmdbg.overlay.ui.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Divider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.smlc666.lkmdbg.data.SessionBridgeState
import com.smlc666.lkmdbg.data.WorkspaceSection

@Composable
fun MainWorkspaceScreen(
    state: SessionBridgeState,
    memoryViewMode: Int,
    memoryToolsOpen: Boolean,
    onSectionSelected: (WorkspaceSection) -> Unit,
    onStepMemoryPage: (Boolean) -> Unit,
    onSelectMemoryAddress: (Long) -> Unit
) {
    Row(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
    ) {
        // Navigation Rail
        Column(
            modifier = Modifier
                .width(80.dp)
                .fillMaxHeight()
                .background(MaterialTheme.colorScheme.surfaceVariant)
        ) {
            val sections = listOf(
                WorkspaceSection.Session,
                WorkspaceSection.Processes,
                WorkspaceSection.Memory,
                WorkspaceSection.Threads,
                WorkspaceSection.Events
            )

            sections.forEach { section ->
                val isSelected = state.workspaceSection == section
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .weight(1f)
                        .background(if (isSelected) MaterialTheme.colorScheme.primaryContainer else Color.Transparent)
                        .clickable { onSectionSelected(section) },
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        text = section.name,
                        color = if (isSelected) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onSurfaceVariant,
                        style = MaterialTheme.typography.bodySmall
                    )
                }
            }
        }

        // Content Area
        Box(
            modifier = Modifier
                .weight(1f)
                .fillMaxHeight()
                .padding(8.dp),
            contentAlignment = Alignment.Center
        ) {
            when (state.workspaceSection) {
                WorkspaceSection.Session -> Text("Session Overview (PID: ${state.snapshot.targetPid})", color = MaterialTheme.colorScheme.onBackground)
                WorkspaceSection.Processes -> Text("Select a process from the picker above.", color = MaterialTheme.colorScheme.onBackground)
                WorkspaceSection.Memory -> MemorySectionContent(state, memoryViewMode, onStepMemoryPage, onSelectMemoryAddress)
                WorkspaceSection.Threads -> Text("Threads not implemented yet.", color = MaterialTheme.colorScheme.onBackground)
                WorkspaceSection.Events -> Text("Events not implemented yet.", color = MaterialTheme.colorScheme.onBackground)
            }
        }
    }
}

@Composable
fun MemorySectionContent(
    state: SessionBridgeState, 
    viewMode: Int,
    onStepPage: (Boolean) -> Unit,
    onSelectAddress: (Long) -> Unit
) {
    Column(modifier = Modifier.fillMaxSize()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Memory View", style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.onBackground)
            Box(modifier = Modifier.weight(1f))
            androidx.compose.material3.Button(onClick = { onStepPage(false) }) {
                Text("Prev Page")
            }
            androidx.compose.material3.Button(onClick = { onStepPage(true) }) {
                Text("Next Page")
            }
        }
        // A placeholder for the complex memory hex viewer
        Box(modifier = Modifier.weight(1f).fillMaxWidth().background(MaterialTheme.colorScheme.surface), contentAlignment = Alignment.Center) {
            Text("Hex dump implementation pending... (Current Mode: ${if (viewMode == 0) "Page" else "Results"})", color = MaterialTheme.colorScheme.onSurface)
        }
    }
}
