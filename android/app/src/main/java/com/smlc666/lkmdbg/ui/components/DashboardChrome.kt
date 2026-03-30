package com.smlc666.lkmdbg.ui.components

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.animateContentSize
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.AutoAwesome
import androidx.compose.material.icons.rounded.Tune
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.data.SessionBridgeState
import com.smlc666.lkmdbg.ui.WorkspaceTab
import com.smlc666.lkmdbg.ui.theme.DeepTeal
import com.smlc666.lkmdbg.ui.theme.Graphite
import com.smlc666.lkmdbg.ui.theme.SignalCyan
import com.smlc666.lkmdbg.ui.theme.Slate

@Composable
internal fun WorkspaceRail(selectedTab: WorkspaceTab, onSelect: (WorkspaceTab) -> Unit) {
    Column(
        modifier = Modifier.padding(start = 10.dp, top = 24.dp, bottom = 24.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        WorkspaceTab.entries.forEach { tab ->
            WorkspaceTabPill(
                tab = tab,
                selected = selectedTab == tab,
                onSelect = onSelect,
            )
        }
    }
}

@Composable
internal fun WorkspaceBar(selectedTab: WorkspaceTab, onSelect: (WorkspaceTab) -> Unit) {
    WorkspaceBar(selectedTab = selectedTab, onSelect = onSelect, modifier = Modifier)
}

@Composable
internal fun WorkspaceBar(
    selectedTab: WorkspaceTab,
    onSelect: (WorkspaceTab) -> Unit,
    modifier: Modifier = Modifier,
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .horizontalScroll(rememberScrollState())
            .padding(vertical = 4.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        WorkspaceTab.entries.forEach { tab ->
            WorkspaceTabPill(
                tab = tab,
                selected = selectedTab == tab,
                onSelect = onSelect,
            )
        }
    }
}

@Composable
private fun WorkspaceTabPill(
    tab: WorkspaceTab,
    selected: Boolean,
    onSelect: (WorkspaceTab) -> Unit,
) {
    val containerColor by animateColorAsState(
        targetValue = if (selected) SignalCyan.copy(alpha = 0.92f) else DeepTeal.copy(alpha = 0.36f),
        animationSpec = tween(durationMillis = 180),
        label = "workspace_tab_container",
    )
    val contentColor by animateColorAsState(
        targetValue = if (selected) Graphite else MaterialTheme.colorScheme.onSurfaceVariant,
        animationSpec = tween(durationMillis = 180),
        label = "workspace_tab_content",
    )
    val borderColor by animateColorAsState(
        targetValue = if (selected) SignalCyan.copy(alpha = 0.2f) else Slate.copy(alpha = 0.18f),
        animationSpec = tween(durationMillis = 180),
        label = "workspace_tab_border",
    )
    val elevation by animateDpAsState(
        targetValue = if (selected) 8.dp else 0.dp,
        animationSpec = tween(durationMillis = 180),
        label = "workspace_tab_elevation",
    )

    Surface(
        onClick = { onSelect(tab) },
        modifier = Modifier
            .animateContentSize()
            .testTag("workspace-tab-${tab.name}"),
        shape = RoundedCornerShape(18.dp),
        color = containerColor,
        contentColor = contentColor,
        tonalElevation = elevation,
        shadowElevation = elevation,
        border = BorderStroke(1.dp, borderColor),
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 14.dp, vertical = 10.dp),
            verticalAlignment = androidx.compose.ui.Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Icon(
                imageVector = tab.icon,
                contentDescription = null,
                modifier = Modifier.size(18.dp),
            )
            Text(
                text = stringResource(tab.titleRes),
                style = MaterialTheme.typography.labelLarge,
                fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Medium,
            )
        }
    }
}

@Composable
internal fun HeroHeader(chips: List<String>, sessionState: SessionBridgeState) {
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
            Row {
                Icon(
                    imageVector = Icons.Rounded.AutoAwesome,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.primary,
                )
                Spacer(Modifier.width(10.dp))
                Text(
                    text = stringResource(R.string.hero_title),
                    style = MaterialTheme.typography.headlineMedium,
                    fontWeight = FontWeight.SemiBold,
                )
            }
            Spacer(Modifier.height(10.dp))
            Text(
                text = stringResource(R.string.hero_subtitle),
                style = MaterialTheme.typography.bodyLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(Modifier.height(18.dp))
            Row(
                modifier = Modifier.horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                chips.plus("agent:${sessionState.agentPath}").forEach { chip ->
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
internal fun StatusStrip(sessionState: SessionBridgeState) {
    Column(
        modifier = Modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        MetricCard(stringResource(R.string.metric_transport), sessionState.snapshot.transport, Modifier.fillMaxWidth())
        MetricCard(
            stringResource(R.string.metric_target),
            "${sessionState.snapshot.targetPid}/${sessionState.snapshot.targetTid}",
            Modifier.fillMaxWidth(),
        )
        MetricCard(stringResource(R.string.metric_agent), sessionState.snapshot.agentPid.toString(), Modifier.fillMaxWidth())
    }
}

@Composable
internal fun MetricCard(title: String, value: String, modifier: Modifier = Modifier) {
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
internal fun PanelCard(
    title: String,
    subtitle: String,
    modifier: Modifier = Modifier,
    titleIconRes: Int? = null,
    content: @Composable () -> Unit,
) {
    Card(
        modifier = modifier,
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surface.copy(alpha = 0.88f),
        ),
        border = BorderStroke(1.dp, Slate.copy(alpha = 0.18f)),
        shape = RoundedCornerShape(28.dp),
    ) {
        Column(
            modifier = Modifier
                .animateContentSize()
                .padding(20.dp),
        ) {
            Row(
                verticalAlignment = androidx.compose.ui.Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                if (titleIconRes != null) {
                    Icon(
                        painter = painterResource(titleIconRes),
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.primary,
                    )
                }
                Text(title, style = MaterialTheme.typography.headlineSmall)
            }
            Spacer(Modifier.height(6.dp))
            Surface(
                color = SignalCyan.copy(alpha = 0.12f),
                shape = RoundedCornerShape(999.dp),
            ) {
                Text(
                    text = subtitle,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 12.dp, vertical = 8.dp),
                )
            }
            Spacer(Modifier.height(18.dp))
            content()
        }
    }
}
