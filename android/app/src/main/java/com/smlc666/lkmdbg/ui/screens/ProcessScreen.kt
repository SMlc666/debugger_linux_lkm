package com.smlc666.lkmdbg.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope.weight
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.data.ProcessFilter
import com.smlc666.lkmdbg.data.ResolvedProcessRecord
import com.smlc666.lkmdbg.data.SessionBridgeState
import com.smlc666.lkmdbg.ui.components.AppProcessIcon
import com.smlc666.lkmdbg.ui.components.PanelCard

@Composable
internal fun ProcessControlPanel(
    state: SessionBridgeState,
    onRefreshProcesses: () -> Unit,
    onProcessFilterChanged: (ProcessFilter) -> Unit,
) {
    val counts = processCounts(state.processes)

    LaunchedEffect(state.processes.isEmpty()) {
        if (state.processes.isEmpty())
            onRefreshProcesses()
    }

    PanelCard(
        title = stringResource(R.string.process_panel_title),
        subtitle = stringResource(R.string.process_panel_subtitle),
    ) {
        Text(
            text = stringResource(
                R.string.process_summary_counts,
                counts.total,
                counts.androidApps,
                counts.commandLine,
                counts.systemApps,
                counts.userApps,
            ),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(14.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            FilledTonalButton(onClick = onRefreshProcesses, enabled = !state.busy) {
                Text(stringResource(R.string.process_action_refresh))
            }
        }
        Spacer(Modifier.height(14.dp))
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            ProcessFilter.entries.forEach { filter ->
                FilterChip(
                    selected = state.processFilter == filter,
                    onClick = { onProcessFilterChanged(filter) },
                    label = { Text(stringResource(filter.labelRes())) },
                )
            }
        }
        if (state.processes.isEmpty()) {
            Spacer(Modifier.height(16.dp))
            Text(
                text = stringResource(R.string.process_empty),
                style = MaterialTheme.typography.bodyLarge,
            )
        }
    }
}

@Composable
internal fun ProcessRowCard(process: ResolvedProcessRecord) {
    Card(
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surface.copy(alpha = 0.82f),
        ),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
            horizontalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            AppProcessIcon(
                packageName = process.iconPackageName,
                displayName = process.displayName,
                modifier = Modifier,
            )
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(
                    text = process.displayName,
                    style = MaterialTheme.typography.titleMedium,
                )
                Text(
                    text = process.processName,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Text(
                    text = stringResource(R.string.process_pid_uid, process.pid, process.uid),
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.primary,
                )
                Text(
                    text = stringResource(R.string.process_kind_label, stringResource(process.kindLabelRes())),
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                process.packageName?.let { packageName ->
                    Text(
                        text = stringResource(R.string.process_package_name, packageName),
                        style = MaterialTheme.typography.labelMedium,
                    )
                }
            }
        }
    }
}

private data class ProcessCounts(
    val total: Int,
    val androidApps: Int,
    val commandLine: Int,
    val systemApps: Int,
    val userApps: Int,
)

private fun processCounts(processes: List<ResolvedProcessRecord>): ProcessCounts =
    ProcessCounts(
        total = processes.size,
        androidApps = processes.count { it.isAndroidApp },
        commandLine = processes.count { !it.isAndroidApp },
        systemApps = processes.count { it.isAndroidApp && it.isSystemApp },
        userApps = processes.count { it.isAndroidApp && !it.isSystemApp },
    )

private fun ProcessFilter.labelRes(): Int =
    when (this) {
        ProcessFilter.All -> R.string.process_filter_all
        ProcessFilter.AndroidApps -> R.string.process_filter_android
        ProcessFilter.CommandLine -> R.string.process_filter_cmdline
        ProcessFilter.SystemApps -> R.string.process_filter_system
        ProcessFilter.UserApps -> R.string.process_filter_user
    }

private fun ResolvedProcessRecord.kindLabelRes(): Int =
    when {
        !isAndroidApp -> R.string.process_kind_command_line
        isSystemApp -> R.string.process_kind_system_app
        else -> R.string.process_kind_user_app
    }
