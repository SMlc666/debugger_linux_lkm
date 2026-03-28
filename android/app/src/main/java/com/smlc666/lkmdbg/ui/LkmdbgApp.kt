package com.smlc666.lkmdbg.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.AutoAwesome
import androidx.compose.material.icons.rounded.Bolt
import androidx.compose.material.icons.rounded.Dns
import androidx.compose.material.icons.rounded.Memory
import androidx.compose.material.icons.rounded.Radar
import androidx.compose.material.icons.rounded.Tune
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.NavigationRail
import androidx.compose.material3.NavigationRailItem
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.smlc666.lkmdbg.shared.BridgeStatusSnapshot
import com.smlc666.lkmdbg.shared.DashboardEvent
import com.smlc666.lkmdbg.shared.DashboardProcess
import com.smlc666.lkmdbg.shared.DashboardThread

private enum class WorkspaceTab(val title: String, val icon: ImageVector) {
    Session("Session", Icons.Rounded.Dns),
    Memory("Memory", Icons.Rounded.Memory),
    Threads("Threads", Icons.Rounded.Bolt),
    Events("Events", Icons.Rounded.Radar),
}

@Composable
fun LkmdbgApp() {
    val state = remember { sampleDashboardState() }
    var selectedTab by remember { mutableStateOf(WorkspaceTab.Session) }

    BoxWithConstraints(
        modifier = Modifier
            .fillMaxSize()
            .background(
                brush = Brush.verticalGradient(
                    colors = listOf(
                        Color(0xFF07111A),
                        Color(0xFF0A1620),
                        Color(0xFF0E1E25),
                    ),
                ),
            ),
    ) {
        val useRail = maxWidth >= 840.dp

        if (useRail) {
            Row(modifier = Modifier.fillMaxSize()) {
                WorkspaceRail(
                    selectedTab = selectedTab,
                    onSelect = { selectedTab = it },
                )
                DashboardContent(
                    state = state,
                    selectedTab = selectedTab,
                    padding = PaddingValues(24.dp),
                )
            }
        } else {
            Column(modifier = Modifier.fillMaxSize()) {
                DashboardContent(
                    state = state,
                    selectedTab = selectedTab,
                    padding = PaddingValues(horizontal = 18.dp, vertical = 18.dp),
                    modifier = Modifier.weight(1f),
                )
                WorkspaceBar(
                    selectedTab = selectedTab,
                    onSelect = { selectedTab = it },
                )
            }
        }
    }
}

@Composable
private fun WorkspaceRail(selectedTab: WorkspaceTab, onSelect: (WorkspaceTab) -> Unit) {
    NavigationRail(
        containerColor = Color.Transparent,
        modifier = Modifier.padding(start = 10.dp, top = 24.dp, bottom = 24.dp),
    ) {
        WorkspaceTab.entries.forEach { tab ->
            NavigationRailItem(
                selected = selectedTab == tab,
                onClick = { onSelect(tab) },
                icon = { Icon(tab.icon, contentDescription = tab.title) },
                label = { Text(tab.title) },
            )
        }
    }
}

@Composable
private fun WorkspaceBar(selectedTab: WorkspaceTab, onSelect: (WorkspaceTab) -> Unit) {
    NavigationBar(containerColor = MaterialTheme.colorScheme.surface.copy(alpha = 0.92f)) {
        WorkspaceTab.entries.forEach { tab ->
            NavigationBarItem(
                selected = selectedTab == tab,
                onClick = { onSelect(tab) },
                icon = { Icon(tab.icon, contentDescription = tab.title) },
                label = { Text(tab.title) },
            )
        }
    }
}

@Composable
private fun DashboardContent(
    state: DashboardState,
    selectedTab: WorkspaceTab,
    padding: PaddingValues,
    modifier: Modifier = Modifier,
) {
    LazyColumn(
        modifier = modifier.fillMaxSize(),
        contentPadding = padding,
        verticalArrangement = Arrangement.spacedBy(18.dp),
    ) {
        item {
            HeroHeader(state = state)
        }
        item {
            StatusStrip(state = state)
        }
        item {
            when (selectedTab) {
                WorkspaceTab.Session -> SessionPane(state)
                WorkspaceTab.Memory -> MemoryPane(state)
                WorkspaceTab.Threads -> ThreadPane(state)
                WorkspaceTab.Events -> EventPane(state)
            }
        }
    }
}

@Composable
private fun HeroHeader(state: DashboardState) {
    Surface(
        shape = RoundedCornerShape(32.dp),
        color = MaterialTheme.colorScheme.surface.copy(alpha = 0.72f),
        tonalElevation = 6.dp,
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .background(
                    Brush.linearGradient(
                        colors = listOf(
                            Color(0xFF0E2935),
                            Color(0xFF113945),
                            Color(0xFF09161F),
                        ),
                    ),
                )
                .padding(24.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(
                    imageVector = Icons.Rounded.AutoAwesome,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.primary,
                )
                Spacer(Modifier.width(10.dp))
                Text(
                    text = "Android GKI Debug Console",
                    style = MaterialTheme.typography.headlineMedium,
                    fontWeight = FontWeight.SemiBold,
                )
            }
            Spacer(Modifier.height(10.dp))
            Text(
                text = "GameGuardian-style control flow on top of root pipe transport. Session bootstrap stays narrow, high-volume actions stay off the UI thread.",
                style = MaterialTheme.typography.bodyLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(Modifier.height(18.dp))
            Row(
                modifier = Modifier.horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                state.chips.forEach { chip ->
                    AssistChip(
                        onClick = {},
                        label = { Text(chip) },
                        leadingIcon = {
                            Icon(
                                imageVector = Icons.Rounded.Tune,
                                contentDescription = null,
                                modifier = Modifier.size(16.dp),
                            )
                        },
                    )
                }
            }
        }
    }
}

@Composable
private fun StatusStrip(state: DashboardState) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        MetricCard("Transport", state.status.transport, Modifier.weight(1f))
        MetricCard("Target", "${state.status.targetPid}/${state.status.targetTid}", Modifier.weight(1f))
        MetricCard("Agent", state.status.agentPid.toString(), Modifier.weight(1f))
    }
}

@Composable
private fun MetricCard(title: String, value: String, modifier: Modifier = Modifier) {
    Card(
        modifier = modifier,
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surface.copy(alpha = 0.82f),
        ),
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Text(
                text = title.uppercase(),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.primary,
            )
            Spacer(Modifier.height(8.dp))
            Text(
                text = value,
                style = MaterialTheme.typography.titleLarge,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}

@Composable
private fun SessionPane(state: DashboardState) {
    Column(verticalArrangement = Arrangement.spacedBy(14.dp)) {
        PanelCard("Attach Queue", "Fast-select a process, then hand control to the root agent over stdio.") {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                state.processes.forEach { process ->
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                    ) {
                        Column(Modifier.weight(1f)) {
                            Text(process.name, style = MaterialTheme.typography.titleMedium)
                            Text(
                                "${process.packageName}  |  pid=${process.pid}  |  ${process.arch}",
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                        AssistChip(onClick = {}, label = { Text(process.state) })
                    }
                }
            }
        }
    }
}

@Composable
private fun MemoryPane(state: DashboardState) {
    Column(verticalArrangement = Arrangement.spacedBy(14.dp)) {
        PanelCard("Memory Workflow", "Search, filter, patch, then pin a watch list without leaving the active target.") {
            Text(
                text = "Recent scan: ${state.scanSummary}",
                style = MaterialTheme.typography.titleMedium,
            )
            Spacer(Modifier.height(12.dp))
            state.memoryRows.forEach { row ->
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                ) {
                    Text(row.first, style = MaterialTheme.typography.bodyLarge)
                    Text(
                        row.second,
                        style = MaterialTheme.typography.bodyLarge,
                        color = MaterialTheme.colorScheme.primary,
                    )
                }
            }
        }
    }
}

@Composable
private fun ThreadPane(state: DashboardState) {
    Column(verticalArrangement = Arrangement.spacedBy(14.dp)) {
        PanelCard("Threads And Registers", "Keep the busy threads visible and surface the instruction pointer first.") {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                state.threads.forEach { thread ->
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                    ) {
                        Column(Modifier.weight(1f)) {
                            Text("${thread.name} (${thread.tid})", style = MaterialTheme.typography.titleMedium)
                            Text(
                                "pc=${thread.pc}",
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                        AssistChip(onClick = {}, label = { Text(thread.state) })
                    }
                }
            }
        }
    }
}

@Composable
private fun EventPane(state: DashboardState) {
    PanelCard("Event Stream", "This is the right place for syscall rule hits, stop reasons, and transport health.") {
        Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
            state.events.forEach { event ->
                Card(
                    colors = CardDefaults.cardColors(
                        containerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.55f),
                    ),
                ) {
                    Column(modifier = Modifier.padding(14.dp)) {
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.SpaceBetween,
                        ) {
                            Text(event.title, style = MaterialTheme.typography.titleMedium)
                            Text(
                                event.timestamp,
                                style = MaterialTheme.typography.labelMedium,
                                color = MaterialTheme.colorScheme.primary,
                            )
                        }
                        Spacer(Modifier.height(6.dp))
                        Text(
                            event.detail,
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun PanelCard(title: String, subtitle: String, content: @Composable () -> Unit) {
    Card(
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surface.copy(alpha = 0.82f),
        ),
        shape = RoundedCornerShape(28.dp),
    ) {
        Column(modifier = Modifier.padding(20.dp)) {
            Text(title, style = MaterialTheme.typography.headlineSmall)
            Spacer(Modifier.height(6.dp))
            Text(
                subtitle,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(Modifier.height(18.dp))
            content()
        }
    }
}

private data class DashboardState(
    val status: BridgeStatusSnapshot,
    val chips: List<String>,
    val processes: List<DashboardProcess>,
    val threads: List<DashboardThread>,
    val events: List<DashboardEvent>,
    val scanSummary: String,
    val memoryRows: List<Pair<String, String>>,
)

private fun sampleDashboardState(): DashboardState =
    DashboardState(
        status = BridgeStatusSnapshot(
            connected = true,
            targetPid = 4321,
            targetTid = 4321,
            sessionOpen = true,
            agentPid = 901,
            transport = "su -> pipe(stdio)",
        ),
        chips = listOf("Root Present", "Pipe Agent", "MD3 Console", "GKI Session"),
        processes = listOf(
            DashboardProcess(4321, "com.example.game", "com.example.game", "arm64", "Attached"),
            DashboardProcess(5544, "surfaceflinger", "system", "arm64", "Idle"),
            DashboardProcess(6110, "zygote64", "system", "arm64", "Available"),
        ),
        threads = listOf(
            DashboardThread(4321, "RenderThread", "0x00000074ab12c090", "Running"),
            DashboardThread(4338, "UnityMain", "0x00000074ab14f5a8", "Stopped"),
            DashboardThread(4349, "Worker-3", "0x00000074ab101c44", "Sleeping"),
        ),
        events = listOf(
            DashboardEvent("Syscall Rule Hit", "openat matched package asset path and rewrote retval for test harness.", "info", "06:42:11"),
            DashboardEvent("Hook State", "proc/version bootstrap active, session fd healthy, no debugfs surface.", "ok", "06:41:58"),
            DashboardEvent("Memory Scan", "4 narrowed results remain after changed-value pass.", "info", "06:41:20"),
        ),
        scanSummary = "Exact 32-bit -> Changed -> Unchanged -> Lock candidates: 4",
        memoryRows = listOf(
            "Watchlist" to "4 entries",
            "Last read" to "128 KiB in 3.1 ms",
            "Last write" to "32 KiB in 0.9 ms",
        ),
    )
