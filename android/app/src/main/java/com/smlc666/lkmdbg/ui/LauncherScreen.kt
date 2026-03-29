package com.smlc666.lkmdbg.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.data.SessionBridgeState
import com.smlc666.lkmdbg.ui.components.MetricCard
import com.smlc666.lkmdbg.ui.components.PanelCard
import com.smlc666.lkmdbg.ui.screens.OverlayControlCard

@Composable
internal fun LauncherScreen(
    dashboardState: DashboardState,
    sessionState: SessionBridgeState,
) {
    val context = LocalContext.current

    Column(
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
            )
            .padding(20.dp),
        verticalArrangement = Arrangement.spacedBy(18.dp),
    ) {
        PanelCard(
            title = context.getString(R.string.launcher_panel_title),
            subtitle = context.getString(R.string.launcher_panel_subtitle),
            titleIconRes = R.drawable.ic_lkmdbg_terminal,
        ) {
            Text(
                text = context.getString(R.string.launcher_panel_body),
                style = MaterialTheme.typography.bodyLarge,
            )
        }

        OverlayControlCard()

        LauncherStatusCard(
            dashboardState = dashboardState,
            sessionState = sessionState,
        )
    }
}

@Composable
private fun LauncherStatusCard(
    dashboardState: DashboardState,
    sessionState: SessionBridgeState,
) {
    val context = LocalContext.current

    PanelCard(
        title = context.getString(R.string.launcher_status_title),
        subtitle = context.getString(R.string.launcher_status_subtitle),
        titleIconRes = R.drawable.ic_lkmdbg_cpu,
    ) {
        Text(
            text = context.getString(R.string.session_last_message, sessionState.lastMessage),
            style = MaterialTheme.typography.bodyMedium,
        )
        Spacer(Modifier.height(16.dp))
        MetricCard(
            title = context.getString(R.string.metric_transport),
            value = sessionState.snapshot.transport,
        )
        Spacer(Modifier.height(10.dp))
        MetricCard(
            title = context.getString(R.string.metric_target),
            value = "${sessionState.snapshot.targetPid}/${sessionState.snapshot.targetTid}",
        )
        Spacer(Modifier.height(10.dp))
        MetricCard(
            title = context.getString(R.string.metric_agent),
            value = sessionState.snapshot.agentPid.toString(),
        )
        Spacer(Modifier.height(10.dp))
        Text(
            text = context.getString(R.string.memory_recent_scan, dashboardState.scanSummary),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}
