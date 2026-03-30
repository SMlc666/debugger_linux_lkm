package com.smlc666.lkmdbg.ui.screens

import androidx.compose.animation.animateContentSize
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.horizontalScroll
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.data.ProcessFilter
import com.smlc666.lkmdbg.data.ResolvedProcessRecord
import com.smlc666.lkmdbg.data.SessionBridgeState
import com.smlc666.lkmdbg.ui.components.AppProcessIcon
import com.smlc666.lkmdbg.ui.components.LkmdbgActionButton
import com.smlc666.lkmdbg.ui.components.LkmdbgFilterPill
import com.smlc666.lkmdbg.ui.components.LkmdbgTag
import com.smlc666.lkmdbg.ui.components.LkmdbgTagTone
import com.smlc666.lkmdbg.ui.components.PanelCard

@Composable
internal fun ProcessWorkspaceScreen(
    state: SessionBridgeState,
    processFilter: ProcessFilter,
    onRefreshProcesses: () -> Unit,
    onProcessFilterChanged: (ProcessFilter) -> Unit,
    onAttachProcess: (Int) -> Unit,
) {
    val filteredProcesses = state.processes.filter { processFilter.matches(it) }

    LazyColumn(
        modifier = Modifier.testTag("workspace-screen-processes"),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        item {
            ProcessControlPanel(
                state = state,
                processFilter = processFilter,
                onRefreshProcesses = onRefreshProcesses,
                onProcessFilterChanged = onProcessFilterChanged,
            )
        }
        if (filteredProcesses.isEmpty()) {
            item {
                Text(
                    text = state.lastMessage.ifBlank { stringResource(R.string.process_empty) },
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        } else {
            items(filteredProcesses, key = { "${it.pid}:${it.processName}" }) { process ->
                ProcessRowCard(
                    process = process,
                    onAttach = { onAttachProcess(process.pid) },
                )
            }
        }
    }
}

@Composable
internal fun ProcessControlPanel(
    state: SessionBridgeState,
    processFilter: ProcessFilter,
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
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .horizontalScroll(rememberScrollState()),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            LkmdbgTag(
                text = stringResource(R.string.process_summary_total, counts.total),
                tone = LkmdbgTagTone.Accent,
            )
            LkmdbgTag(text = stringResource(R.string.process_summary_android, counts.androidApps))
            LkmdbgTag(text = stringResource(R.string.process_summary_cmd, counts.commandLine))
            LkmdbgTag(text = stringResource(R.string.process_summary_system, counts.systemApps))
            LkmdbgTag(text = stringResource(R.string.process_summary_user, counts.userApps))
        }
        Spacer(Modifier.height(14.dp))
        LkmdbgActionButton(
            text = stringResource(R.string.process_action_refresh),
            onClick = onRefreshProcesses,
            enabled = !state.busy,
            prominent = true,
        )
        Spacer(Modifier.height(14.dp))
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .horizontalScroll(rememberScrollState()),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            ProcessFilter.entries.forEach { filter ->
                LkmdbgFilterPill(
                    text = stringResource(filter.labelRes()),
                    selected = processFilter == filter,
                    modifier = Modifier.testTag("process-filter-${filter.name}"),
                    onClick = { onProcessFilterChanged(filter) },
                )
            }
        }
    }
}

@Composable
internal fun ProcessRowCard(process: ResolvedProcessRecord, onAttach: () -> Unit) {
    Card(
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surface.copy(alpha = 0.94f),
        ),
    ) {
        Row(
            modifier = Modifier
                .animateContentSize()
                .fillMaxWidth()
                .padding(16.dp),
            horizontalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            AppProcessIcon(
                packageName = process.iconPackageName,
                displayName = process.displayName,
            )
            Column(
                modifier = Modifier.weight(1f),
                verticalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                ) {
                    Column(modifier = Modifier.weight(1f)) {
                        Text(
                            text = process.displayName,
                            style = MaterialTheme.typography.titleMedium,
                        )
                        Text(
                            text = process.processName,
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    LkmdbgActionButton(
                        text = stringResource(R.string.process_action_attach),
                        onClick = onAttach,
                        prominent = true,
                    )
                }
                Row(
                    modifier = Modifier.horizontalScroll(rememberScrollState()),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    LkmdbgTag(
                        text = stringResource(R.string.process_pid_uid, process.pid, process.uid),
                        tone = LkmdbgTagTone.Positive,
                    )
                    LkmdbgTag(
                        text = stringResource(process.kindLabelRes()),
                        tone = if (process.isAndroidApp) LkmdbgTagTone.Accent else LkmdbgTagTone.Neutral,
                    )
                }
                process.packageName?.let { packageName ->
                    Surface(
                        color = MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.38f),
                        shape = androidx.compose.foundation.shape.RoundedCornerShape(12.dp),
                    ) {
                        Text(
                            text = stringResource(R.string.process_package_name, packageName),
                            style = MaterialTheme.typography.labelMedium,
                            color = MaterialTheme.colorScheme.primary,
                            modifier = Modifier.padding(horizontal = 10.dp, vertical = 6.dp),
                        )
                    }
                }
                Text(
                    text = process.cmdline.ifBlank { process.comm },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
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
