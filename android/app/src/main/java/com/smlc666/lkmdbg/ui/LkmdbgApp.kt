package com.smlc666.lkmdbg.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.smlc666.lkmdbg.data.ProcessFilter
import com.smlc666.lkmdbg.data.SessionBridgeRepository
import com.smlc666.lkmdbg.ui.components.HeroHeader
import com.smlc666.lkmdbg.ui.components.StatusStrip
import com.smlc666.lkmdbg.ui.components.WorkspaceBar
import com.smlc666.lkmdbg.ui.components.WorkspaceRail
import com.smlc666.lkmdbg.ui.screens.EventScreen
import com.smlc666.lkmdbg.ui.screens.MemoryScreen
import com.smlc666.lkmdbg.ui.screens.ProcessControlPanel
import com.smlc666.lkmdbg.ui.screens.ProcessRowCard
import com.smlc666.lkmdbg.ui.screens.SessionScreen
import com.smlc666.lkmdbg.ui.screens.ThreadScreen
import kotlinx.coroutines.launch

@Composable
fun LkmdbgApp(repository: SessionBridgeRepository) {
    val context = LocalContext.current
    val dashboardState = remember(context) { sampleDashboardState(context) }
    val sessionState by repository.state.collectAsStateWithLifecycle()
    val coroutineScope = rememberCoroutineScope()
    var selectedTab by rememberSaveable { androidx.compose.runtime.mutableStateOf(WorkspaceTab.Session) }

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
                WorkspaceRail(selectedTab = selectedTab, onSelect = { selectedTab = it })
                DashboardContent(
                    dashboardState = dashboardState,
                    sessionState = sessionState,
                    selectedTab = selectedTab,
                    padding = PaddingValues(24.dp),
                    modifier = Modifier.fillMaxSize(),
                    onConnect = { coroutineScope.launch { repository.connect() } },
                    onOpenSession = { coroutineScope.launch { repository.openSession() } },
                    onRefreshStatus = { coroutineScope.launch { repository.refreshStatus() } },
                    onAttachTarget = { coroutineScope.launch { repository.attachTarget() } },
                    onRefreshProcesses = { coroutineScope.launch { repository.refreshProcesses() } },
                    onRefreshThreads = { coroutineScope.launch { repository.refreshThreads() } },
                    onRefreshEvents = { coroutineScope.launch { repository.refreshEvents() } },
                    onRefreshImages = { coroutineScope.launch { repository.refreshImages() } },
                    onPreviewSelectedPc = { coroutineScope.launch { repository.previewSelectedPc() } },
                    onAttachProcess = { processPid ->
                        coroutineScope.launch {
                            if (repository.attachProcess(processPid))
                                selectedTab = WorkspaceTab.Threads
                        }
                    },
                    onSelectThread = { tid ->
                        coroutineScope.launch { repository.refreshThreadRegisters(tid) }
                    },
                    onTargetPidChanged = repository::updateTargetPidInput,
                    onProcessFilterChanged = repository::updateProcessFilter,
                )
            }
        } else {
            Box(modifier = Modifier.fillMaxSize()) {
                DashboardContent(
                    dashboardState = dashboardState,
                    sessionState = sessionState,
                    selectedTab = selectedTab,
                    padding = PaddingValues(start = 18.dp, top = 18.dp, end = 18.dp, bottom = 96.dp),
                    modifier = Modifier.fillMaxSize(),
                    onConnect = { coroutineScope.launch { repository.connect() } },
                    onOpenSession = { coroutineScope.launch { repository.openSession() } },
                    onRefreshStatus = { coroutineScope.launch { repository.refreshStatus() } },
                    onAttachTarget = { coroutineScope.launch { repository.attachTarget() } },
                    onRefreshProcesses = { coroutineScope.launch { repository.refreshProcesses() } },
                    onRefreshThreads = { coroutineScope.launch { repository.refreshThreads() } },
                    onRefreshEvents = { coroutineScope.launch { repository.refreshEvents() } },
                    onRefreshImages = { coroutineScope.launch { repository.refreshImages() } },
                    onPreviewSelectedPc = { coroutineScope.launch { repository.previewSelectedPc() } },
                    onAttachProcess = { processPid ->
                        coroutineScope.launch {
                            if (repository.attachProcess(processPid))
                                selectedTab = WorkspaceTab.Threads
                        }
                    },
                    onSelectThread = { tid ->
                        coroutineScope.launch { repository.refreshThreadRegisters(tid) }
                    },
                    onTargetPidChanged = repository::updateTargetPidInput,
                    onProcessFilterChanged = repository::updateProcessFilter,
                )
                WorkspaceBar(
                    selectedTab = selectedTab,
                    onSelect = { selectedTab = it },
                    modifier = Modifier.align(Alignment.BottomCenter),
                )
            }
        }
    }
}

@Composable
private fun DashboardContent(
    dashboardState: DashboardState,
    sessionState: com.smlc666.lkmdbg.data.SessionBridgeState,
    selectedTab: WorkspaceTab,
    padding: PaddingValues,
    onConnect: () -> Unit,
    onOpenSession: () -> Unit,
    onRefreshStatus: () -> Unit,
    onAttachTarget: () -> Unit,
    onRefreshProcesses: () -> Unit,
    onRefreshThreads: () -> Unit,
    onRefreshEvents: () -> Unit,
    onRefreshImages: () -> Unit,
    onPreviewSelectedPc: () -> Unit,
    onAttachProcess: (Int) -> Unit,
    onSelectThread: (Int) -> Unit,
    onTargetPidChanged: (String) -> Unit,
    onProcessFilterChanged: (ProcessFilter) -> Unit,
    modifier: Modifier = Modifier,
) {
    val filteredProcesses = remember(sessionState.processes, sessionState.processFilter) {
        sessionState.processes.filter { sessionState.processFilter.matches(it) }
    }

    LazyColumn(
        modifier = modifier.fillMaxSize(),
        contentPadding = padding,
        verticalArrangement = Arrangement.spacedBy(18.dp),
    ) {
        item { HeroHeader(chips = dashboardState.chips, sessionState = sessionState) }
        item { StatusStrip(sessionState) }
        item {
            when (selectedTab) {
                WorkspaceTab.Session -> SessionScreen(
                    state = sessionState,
                    onConnect = onConnect,
                    onOpenSession = onOpenSession,
                    onRefreshStatus = onRefreshStatus,
                    onAttachTarget = onAttachTarget,
                    onTargetPidChanged = onTargetPidChanged,
                )
                WorkspaceTab.Processes -> {
                    ProcessControlPanel(
                        state = sessionState,
                        onRefreshProcesses = onRefreshProcesses,
                        onProcessFilterChanged = onProcessFilterChanged,
                    )
                }
                WorkspaceTab.Memory -> MemoryScreen(
                    dashboardState = dashboardState,
                    state = sessionState,
                    onRefreshImages = onRefreshImages,
                    onPreviewSelectedPc = onPreviewSelectedPc,
                )
                WorkspaceTab.Threads -> ThreadScreen(
                    state = sessionState,
                    onRefreshThreads = onRefreshThreads,
                    onSelectThread = onSelectThread,
                )
                WorkspaceTab.Events -> EventScreen(
                    state = sessionState,
                    onRefreshEvents = onRefreshEvents,
                )
            }
        }
        if (selectedTab == WorkspaceTab.Processes) {
            items(filteredProcesses, key = { "${it.pid}:${it.processName}" }) { process ->
                ProcessRowCard(process = process, onAttach = { onAttachProcess(process.pid) })
            }
        }
    }
}
