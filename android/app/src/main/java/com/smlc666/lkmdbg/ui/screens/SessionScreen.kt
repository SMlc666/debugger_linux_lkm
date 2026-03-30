package com.smlc666.lkmdbg.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.weight
import androidx.compose.material3.Button
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.TextButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.smlc666.lkmdbg.data.ProcessFilter
import com.smlc666.lkmdbg.data.ResolvedProcessRecord
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.data.SessionBridgeState
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

    Column(verticalArrangement = Arrangement.spacedBy(14.dp)) {
        SessionControlCard(
            state = state,
            onConnect = onConnect,
            onOpenSession = onOpenSession,
            onRefreshStatus = onRefreshStatus,
            onAttachTarget = onAttachTarget,
            onTargetPidChanged = onTargetPidChanged,
        )
        QuickAttachCard(
            state = state,
            filteredProcesses = filteredProcesses,
            onRefreshProcesses = onRefreshProcesses,
            onProcessFilterChanged = onProcessFilterChanged,
            onAttachProcess = onAttachProcess,
        )
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
        Text(text = stringResource(R.string.session_last_message, state.lastMessage))
        Spacer(Modifier.height(14.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            FilledTonalButton(onClick = onConnect, enabled = !state.busy) {
                Text(stringResource(R.string.session_action_connect))
            }
            Button(onClick = onOpenSession, enabled = !state.busy) {
                Text(stringResource(R.string.session_action_open_session))
            }
            FilledTonalButton(onClick = onRefreshStatus, enabled = !state.busy) {
                Text(stringResource(R.string.session_action_refresh))
            }
        }
        Spacer(Modifier.height(14.dp))
        OutlinedTextField(
            value = state.targetPidInput,
            onValueChange = onTargetPidChanged,
            enabled = !state.busy,
            singleLine = true,
            label = { Text(stringResource(R.string.session_target_pid)) },
            modifier = Modifier.fillMaxWidth(),
        )
        Spacer(Modifier.height(12.dp))
        Button(onClick = onAttachTarget, enabled = !state.busy) {
            Text(stringResource(R.string.session_action_attach_target))
        }
        Spacer(Modifier.height(18.dp))
        Text(stringResource(R.string.session_flag_open, state.snapshot.sessionOpen))
        Text(stringResource(R.string.session_flag_connected, state.snapshot.connected))
        Text(stringResource(R.string.session_flag_hook_active, state.snapshot.hookActive))
        Text(stringResource(R.string.session_flag_owner_pid, state.snapshot.ownerPid))
        Text(stringResource(R.string.session_flag_event_queue, state.snapshot.eventQueueDepth.toString()))
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
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            FilledTonalButton(onClick = onRefreshProcesses, enabled = !state.busy) {
                Text(stringResource(R.string.process_action_refresh))
            }
        }
        Spacer(Modifier.height(12.dp))
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            ProcessFilter.entries.forEach { filter ->
                FilterChip(
                    selected = state.processFilter == filter,
                    onClick = { onProcessFilterChanged(filter) },
                    label = { Text(stringResource(filter.labelRes())) },
                )
            }
        }
        Spacer(Modifier.height(12.dp))
        if (filteredProcesses.isEmpty()) {
            Text(
                text = stringResource(R.string.process_empty),
            )
        } else {
            filteredProcesses.take(8).forEach { process ->
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 4.dp),
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    Column(modifier = Modifier.weight(1f)) {
                        Text(process.displayName)
                        Text(
                            text = process.processName,
                        )
                        Text(
                            text = stringResource(R.string.process_pid_uid, process.pid, process.uid),
                        )
                    }
                    TextButton(
                        onClick = { onAttachProcess(process.pid) },
                        enabled = !state.busy,
                    ) {
                        Text(stringResource(R.string.process_action_attach))
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
