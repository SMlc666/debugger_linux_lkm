package com.smlc666.lkmdbg.overlay

import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.positionChange
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.runtime.collectAsState
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.data.SessionBridgeRepository
import com.smlc666.lkmdbg.ui.WorkspaceContent
import com.smlc666.lkmdbg.ui.WorkspaceTab
import com.smlc666.lkmdbg.ui.rememberWorkspaceActions
import com.smlc666.lkmdbg.ui.sampleDashboardState
import com.smlc666.lkmdbg.ui.components.WorkspaceRail
import kotlin.math.hypot

@Composable
internal fun OverlayWorkspace(
    repository: SessionBridgeRepository,
    expanded: Boolean,
    onExpand: () -> Unit,
    onCollapse: () -> Unit,
    onMoveBallBy: (Float, Float) -> Unit,
    onClose: () -> Unit,
) {
    val context = LocalContext.current
    val dashboardState = remember(context) { sampleDashboardState(context) }
    val sessionState by repository.state.collectAsState()
    var selectedTab by remember { mutableStateOf(WorkspaceTab.Session) }
    val actions = rememberWorkspaceActions(repository) {
        selectedTab = WorkspaceTab.Threads
    }

    if (!expanded) {
        FloatingBall(
            onExpand = onExpand,
            onMoveBy = onMoveBallBy,
        )
        return
    }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black.copy(alpha = 0.52f))
            .padding(horizontal = 12.dp, vertical = 10.dp),
    ) {
        BoxWithConstraints(modifier = Modifier.fillMaxSize()) {
            val wideLayout = maxWidth >= 840.dp || maxWidth > maxHeight
            val tabletLayout = maxWidth >= 1100.dp
            val surfaceModifier = if (tabletLayout) {
                Modifier
                    .fillMaxSize()
                    .padding(horizontal = 56.dp, vertical = 18.dp)
                    .widthIn(max = 1280.dp)
            } else if (wideLayout) {
                Modifier
                    .fillMaxSize()
                    .padding(horizontal = 20.dp, vertical = 10.dp)
            } else {
                Modifier.fillMaxSize()
            }

            Box(
                modifier = Modifier.fillMaxSize(),
                contentAlignment = Alignment.Center,
            ) {
                Surface(
                    modifier = surfaceModifier,
                    shape = RoundedCornerShape(if (wideLayout) 30.dp else 24.dp),
                    color = MaterialTheme.colorScheme.surface.copy(alpha = 0.94f),
                    tonalElevation = 18.dp,
                    shadowElevation = 18.dp,
                ) {
                    Column(
                        modifier = Modifier
                            .fillMaxSize()
                            .background(MaterialTheme.colorScheme.surface.copy(alpha = 0.9f)),
                    ) {
                        ExpandedOverlayHeader(
                            selectedTab = selectedTab,
                            transport = sessionState.snapshot.transport,
                            targetPid = sessionState.snapshot.targetPid,
                            targetTid = sessionState.snapshot.targetTid,
                            sessionId = sessionState.snapshot.sessionId,
                            landscape = wideLayout,
                            onCollapse = onCollapse,
                            onClose = onClose,
                        )
                        if (wideLayout) {
                            Row(modifier = Modifier.fillMaxSize()) {
                                WorkspaceRail(
                                    selectedTab = selectedTab,
                                    onSelect = { selectedTab = it },
                                )
                                WorkspaceContent(
                                    dashboardState = dashboardState,
                                    sessionState = sessionState,
                                    selectedTab = selectedTab,
                                    onSelectTab = { selectedTab = it },
                                    actions = actions,
                                    modifier = Modifier.weight(1f),
                                    contentPadding = PaddingValues(horizontal = 14.dp, vertical = 12.dp),
                                    showStatusStrip = false,
                                    showWorkspaceBar = false,
                                )
                            }
                        } else {
                            WorkspaceContent(
                                dashboardState = dashboardState,
                                sessionState = sessionState,
                                selectedTab = selectedTab,
                                onSelectTab = { selectedTab = it },
                                actions = actions,
                                modifier = Modifier.weight(1f),
                                contentPadding = PaddingValues(horizontal = 10.dp, vertical = 10.dp),
                                showStatusStrip = false,
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun FloatingBall(
    onExpand: () -> Unit,
    onMoveBy: (Float, Float) -> Unit,
) {
    Surface(
        modifier = Modifier
            .size(72.dp)
            .pointerInput(onExpand, onMoveBy) {
                awaitEachGesture {
                    awaitFirstDown(requireUnconsumed = false)
                    var totalDelta = Offset.Zero
                    var dragged = false

                    do {
                        val event = awaitPointerEvent()
                        val change = event.changes.firstOrNull() ?: break
                        val delta = change.positionChange()
                        if (delta != Offset.Zero) {
                            totalDelta += delta
                            if (!dragged && hypot(totalDelta.x.toDouble(), totalDelta.y.toDouble()) > viewConfiguration.touchSlop)
                                dragged = true
                            if (dragged) {
                                onMoveBy(delta.x, delta.y)
                            }
                        }
                    } while (event.changes.any { it.pressed })

                    if (!dragged && hypot(totalDelta.x.toDouble(), totalDelta.y.toDouble()) <= viewConfiguration.touchSlop)
                        onExpand()
                }
            },
        shape = CircleShape,
        color = MaterialTheme.colorScheme.primary.copy(alpha = 0.94f),
        tonalElevation = 12.dp,
        shadowElevation = 16.dp,
    ) {
        Box(
            modifier = Modifier.fillMaxSize(),
            contentAlignment = Alignment.Center,
        ) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Icon(
                    painter = painterResource(R.drawable.ic_lkmdbg_radar),
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.onPrimary,
                )
                Text(
                    text = stringResource(R.string.overlay_ball_label),
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold,
                    color = MaterialTheme.colorScheme.onPrimary,
                )
                Text(
                    text = stringResource(R.string.overlay_drag_handle),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onPrimary.copy(alpha = 0.82f),
                )
            }
        }
    }
}

@Composable
private fun ExpandedOverlayHeader(
    selectedTab: WorkspaceTab,
    transport: String,
    targetPid: Int,
    targetTid: Int,
    sessionId: ULong,
    landscape: Boolean,
    onCollapse: () -> Unit,
    onClose: () -> Unit,
) {
    val layoutModifier = Modifier
        .fillMaxWidth()
        .background(MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.68f))
        .padding(horizontal = if (landscape) 18.dp else 14.dp, vertical = 12.dp)

    if (landscape) {
        Row(
            modifier = layoutModifier,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            HeaderText(
                selectedTab = selectedTab,
                transport = transport,
                targetPid = targetPid,
                targetTid = targetTid,
                sessionId = sessionId,
                modifier = Modifier.weight(1f),
            )
            Spacer(Modifier.width(10.dp))
            FilledTonalButton(onClick = onCollapse) {
                Text(stringResource(R.string.overlay_action_collapse))
            }
            Spacer(Modifier.width(8.dp))
            FilledTonalButton(onClick = onClose) {
                Text(stringResource(R.string.overlay_action_close))
            }
        }
    } else {
        Column(
            modifier = layoutModifier,
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            HeaderText(
                selectedTab = selectedTab,
                transport = transport,
                targetPid = targetPid,
                targetTid = targetTid,
                sessionId = sessionId,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilledTonalButton(onClick = onCollapse) {
                    Text(stringResource(R.string.overlay_action_collapse))
                }
                FilledTonalButton(onClick = onClose) {
                    Text(stringResource(R.string.overlay_action_close))
                }
            }
        }
    }
}

@Composable
private fun HeaderText(
    selectedTab: WorkspaceTab,
    transport: String,
    targetPid: Int,
    targetTid: Int,
    sessionId: ULong,
    modifier: Modifier = Modifier,
) {
    Column(
        modifier = modifier,
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Icon(
                painter = painterResource(R.drawable.ic_lkmdbg_terminal),
                contentDescription = null,
                tint = MaterialTheme.colorScheme.primary,
            )
            Text(
                text = stringResource(
                    R.string.overlay_title_with_section,
                    stringResource(selectedTab.titleRes),
                ),
                style = MaterialTheme.typography.titleMedium,
            )
        }
        Text(
            text = stringResource(
                R.string.overlay_status_template,
                transport,
                targetPid,
                targetTid,
                "0x${sessionId.toString(16)}",
            ),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}
