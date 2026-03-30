package com.smlc666.lkmdbg.ui.screens

import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.weight
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
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
import com.smlc666.lkmdbg.ui.components.LkmdbgActionButton
import com.smlc666.lkmdbg.ui.components.LkmdbgFilterPill
import com.smlc666.lkmdbg.ui.components.LkmdbgInputField
import com.smlc666.lkmdbg.ui.components.LkmdbgTag
import com.smlc666.lkmdbg.ui.components.LkmdbgTagTone
import com.smlc666.lkmdbg.ui.components.PanelCard

@Composable
internal fun SessionScreen(
    state: SessionBridgeState,
    onConnect: () -> Unit,
    onOpenSession: () -> Unit,
    onRefreshStatus: () -> Unit,
    onAttachTarget: () -> Unit,
    onTargetPidChanged: (String) -> Unit,
    onRefreshProcesses: () -> Unit,
    onProcessFilterChanged: (ProcessFilter) -> Unit,
    onAttachProcess: (Int) -> Unit,
) {
    val filteredProcesses = state.processes.filter { state.processFilter.matches(it) }

    LazyColumn(
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        item {
            SessionControlCard(
                state = state,
                onConnect = onConnect,
                onOpenSession = onOpenSession,
                onRefreshStatus = onRefreshStatus,
                onAttachTarget = onAttachTarget,
                onTargetPidChanged = onTargetPidChanged,
            )
        }
        item {
            QuickAttachCard(
                state = state,
                filteredProcesses = filteredProcesses,
                onRefreshProcesses = onRefreshProcesses,
                onProcessFilterChanged = onProcessFilterChanged,
                onAttachProcess = onAttachProcess,
            )
        }
        item {
            PanelCard(
                title = stringResource(R.string.session_attach_queue_title),
                subtitle = stringResource(R.string.session_attach_queue_subtitle),
            ) {
                Text(
                    text = stringResource(R.string.session_attach_queue_detail),
                )
            }
        }
    }
}

@Composable
private fun SessionControlCard(
    state: SessionBridgeState,
    onConnect: () -> Unit,
    onOpenSession: () -> Unit,
    onRefreshStatus: () -> Unit,
    onAttachTarget: () -> Unit,
    onTargetPidChanged: (String) -> Unit,
) {
    PanelCard(
        title = stringResource(R.string.session_panel_title),
        subtitle = stringResource(R.string.session_panel_subtitle),
    ) {
        Text(
            text = stringResource(R.string.session_last_message, state.lastMessage),
            style = MaterialTheme.typography.bodyMedium,
        )
        Spacer(Modifier.height(14.dp))
        Row(
            modifier = Modifier.horizontalScroll(rememberScrollState()),
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            LkmdbgActionButton(
                text = stringResource(R.string.session_action_connect),
                onClick = onConnect,
                enabled = !state.busy,
            )
            LkmdbgActionButton(
                text = stringResource(R.string.session_action_open_session),
                onClick = onOpenSession,
                enabled = !state.busy,
                prominent = true,
            )
            LkmdbgActionButton(
                text = stringResource(R.string.session_action_refresh),
                onClick = onRefreshStatus,
                enabled = !state.busy,
            )
        }
        Spacer(Modifier.height(14.dp))
        LkmdbgInputField(
            value = state.targetPidInput,
            onValueChange = onTargetPidChanged,
            label = stringResource(R.string.session_target_pid),
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )
        Spacer(Modifier.height(12.dp))
        LkmdbgActionButton(
            text = stringResource(R.string.session_action_attach_target),
            onClick = onAttachTarget,
            enabled = !state.busy,
            prominent = true,
        )
        Spacer(Modifier.height(18.dp))
        Row(
            modifier = Modifier.horizontalScroll(rememberScrollState()),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            LkmdbgTag(
                text = stringResource(R.string.session_flag_open, state.snapshot.sessionOpen),
                tone = LkmdbgTagTone.Accent,
            )
            LkmdbgTag(
                text = stringResource(R.string.session_flag_connected, state.snapshot.connected),
                tone = LkmdbgTagTone.Positive,
            )
            LkmdbgTag(text = stringResource(R.string.session_flag_hook_active, state.snapshot.hookActive))
            LkmdbgTag(text = stringResource(R.string.session_flag_owner_pid, state.snapshot.ownerPid))
            LkmdbgTag(text = stringResource(R.string.session_flag_event_queue, state.snapshot.eventQueueDepth.toString()))
        }
    }
}

@Composable
private fun QuickAttachCard(
    state: SessionBridgeState,
    filteredProcesses: List<ResolvedProcessRecord>,
    onRefreshProcesses: () -> Unit,
    onProcessFilterChanged: (ProcessFilter) -> Unit,
    onAttachProcess: (Int) -> Unit,
) {
    LaunchedEffect(state.processes.isEmpty()) {
        if (state.processes.isEmpty())
            onRefreshProcesses()
    }

    PanelCard(
        title = stringResource(R.string.session_quick_attach_title),
        subtitle = stringResource(R.string.session_quick_attach_subtitle),
    ) {
        LkmdbgActionButton(
            text = stringResource(R.string.process_action_refresh),
            onClick = onRefreshProcesses,
            enabled = !state.busy,
            prominent = true,
        )
        Spacer(Modifier.height(12.dp))
        Row(
            modifier = Modifier.horizontalScroll(rememberScrollState()),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            ProcessFilter.entries.forEach { filter ->
                LkmdbgFilterPill(
                    text = stringResource(filter.labelRes()),
                    selected = state.processFilter == filter,
                    onClick = { onProcessFilterChanged(filter) },
                )
            }
        }
        Spacer(Modifier.height(12.dp))
        if (filteredProcesses.isEmpty()) {
            Text(
                text = stringResource(R.string.process_empty),
            )
        } else {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                filteredProcesses.take(8).forEach { process ->
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(vertical = 2.dp),
                        horizontalArrangement = Arrangement.spacedBy(10.dp),
                    ) {
                        AppProcessIcon(
                            packageName = process.iconPackageName,
                            displayName = process.displayName,
                        )
                        Column(modifier = Modifier.weight(1f)) {
                            Text(
                                process.displayName,
                                style = MaterialTheme.typography.titleMedium,
                            )
                            Text(
                                text = process.processName,
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                LkmdbgTag(
                                    text = stringResource(R.string.process_pid_uid, process.pid, process.uid),
                                    tone = LkmdbgTagTone.Positive,
                                )
                                if (process.isAndroidApp) {
                                    LkmdbgTag(
                                        text = stringResource(process.kindLabelRes()),
                                        tone = LkmdbgTagTone.Accent,
                                    )
                                }
                            }
                        }
                        LkmdbgActionButton(
                            text = stringResource(R.string.process_action_attach),
                            onClick = { onAttachProcess(process.pid) },
                            enabled = !state.busy,
                        )
                    }
                }
            }
        }
    }
}

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
