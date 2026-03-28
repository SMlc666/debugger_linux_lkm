package com.smlc666.lkmdbg.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.data.SessionBridgeState
import com.smlc666.lkmdbg.ui.components.PanelCard

@Composable
internal fun EventScreen(
    state: SessionBridgeState,
    onRefreshEvents: () -> Unit,
) {
    LaunchedEffect(state.snapshot.sessionOpen, state.recentEvents.isEmpty()) {
        if (state.snapshot.sessionOpen && state.recentEvents.isEmpty())
            onRefreshEvents()
    }

    PanelCard(
        title = stringResource(R.string.event_panel_title),
        subtitle = stringResource(R.string.event_panel_subtitle),
    ) {
        FilledTonalButton(onClick = onRefreshEvents, enabled = !state.busy) {
            Text(stringResource(R.string.event_action_refresh))
        }
        Spacer(Modifier.height(12.dp))
        if (state.recentEvents.isEmpty()) {
            Text(stringResource(R.string.event_empty))
            return@PanelCard
        }
        Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
            state.recentEvents.forEach { entry ->
                Card(
                    colors = CardDefaults.cardColors(
                        containerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.55f),
                    ),
                ) {
                    Column(modifier = Modifier.padding(14.dp)) {
                        Text(eventTypeLabel(entry.record.type), style = MaterialTheme.typography.titleMedium)
                        Spacer(Modifier.height(6.dp))
                        Text(
                            stringResource(
                                R.string.event_detail_template,
                                entry.record.seq.toString(),
                                entry.record.tgid,
                                entry.record.tid,
                                entry.record.code.toString(),
                                entry.record.flags.toString(),
                                entry.record.value0.toString(),
                                entry.record.value1.toString(),
                            ),
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        Spacer(Modifier.height(4.dp))
                        Text(
                            stringResource(R.string.event_received_template, entry.receivedAtMs.toString()),
                            style = MaterialTheme.typography.labelMedium,
                            color = MaterialTheme.colorScheme.primary,
                            modifier = Modifier.fillMaxWidth(),
                        )
                    }
                }
            }
        }
    }
}

private fun eventTypeLabel(type: UInt): String =
    when (type.toInt()) {
        1 -> "SESSION_OPENED"
        2 -> "SESSION_RESET"
        3 -> "INTERNAL_NOTICE"
        16 -> "HOOK_INSTALLED"
        17 -> "HOOK_REMOVED"
        18 -> "HOOK_HIT"
        32 -> "TARGET_CLONE"
        33 -> "TARGET_EXEC"
        34 -> "TARGET_EXIT"
        35 -> "TARGET_SIGNAL"
        36 -> "TARGET_STOP"
        37 -> "TARGET_SYSCALL"
        38 -> "TARGET_MMAP"
        39 -> "TARGET_MUNMAP"
        40 -> "TARGET_MPROTECT"
        41 -> "TARGET_SYSCALL_RULE"
        42 -> "TARGET_SYSCALL_RULE_DETAIL"
        else -> "EVENT_$type"
    }
