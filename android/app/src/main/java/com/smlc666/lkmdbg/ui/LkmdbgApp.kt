package com.smlc666.lkmdbg.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.weight
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.smlc666.lkmdbg.data.SessionBridgeRepository
import com.smlc666.lkmdbg.ui.components.HeroHeader
import com.smlc666.lkmdbg.ui.components.StatusStrip
import com.smlc666.lkmdbg.ui.components.WorkspaceBar
import com.smlc666.lkmdbg.ui.components.WorkspaceRail
import com.smlc666.lkmdbg.ui.screens.EventScreen
import com.smlc666.lkmdbg.ui.screens.MemoryScreen
import com.smlc666.lkmdbg.ui.screens.ProcessSummaryScreen
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
                    sessionRepository = repository,
                    sessionState = sessionState,
                    selectedTab = selectedTab,
                    padding = PaddingValues(24.dp),
                    modifier = Modifier.weight(1f),
                    onConnect = { coroutineScope.launch { repository.connect() } },
                    onOpenSession = { coroutineScope.launch { repository.openSession() } },
                    onRefreshStatus = { coroutineScope.launch { repository.refreshStatus() } },
                    onAttachTarget = { coroutineScope.launch { repository.attachTarget() } },
                    onTargetPidChanged = repository::updateTargetPidInput,
                )
            }
        } else {
            Column(modifier = Modifier.fillMaxSize()) {
                DashboardContent(
                    dashboardState = dashboardState,
                    sessionRepository = repository,
                    sessionState = sessionState,
                    selectedTab = selectedTab,
                    padding = PaddingValues(horizontal = 18.dp, vertical = 18.dp),
                    modifier = Modifier.weight(1f),
                    onConnect = { coroutineScope.launch { repository.connect() } },
                    onOpenSession = { coroutineScope.launch { repository.openSession() } },
                    onRefreshStatus = { coroutineScope.launch { repository.refreshStatus() } },
                    onAttachTarget = { coroutineScope.launch { repository.attachTarget() } },
                    onTargetPidChanged = repository::updateTargetPidInput,
                )
                WorkspaceBar(selectedTab = selectedTab, onSelect = { selectedTab = it })
            }
        }
    }
}

@Composable
private fun DashboardContent(
    dashboardState: DashboardState,
    sessionRepository: SessionBridgeRepository,
    sessionState: com.smlc666.lkmdbg.data.SessionBridgeState,
    selectedTab: WorkspaceTab,
    padding: PaddingValues,
    onConnect: () -> Unit,
    onOpenSession: () -> Unit,
    onRefreshStatus: () -> Unit,
    onAttachTarget: () -> Unit,
    onTargetPidChanged: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
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
                WorkspaceTab.Memory -> MemoryScreen(dashboardState)
                WorkspaceTab.Threads -> {
                    Column(verticalArrangement = Arrangement.spacedBy(14.dp)) {
                        ProcessSummaryScreen(dashboardState)
                        ThreadScreen(dashboardState)
                    }
                }
                WorkspaceTab.Events -> EventScreen(dashboardState)
            }
        }
    }
}
