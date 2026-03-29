package com.smlc666.lkmdbg.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.material3.Button
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
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
) {
    Column(verticalArrangement = Arrangement.spacedBy(14.dp)) {
        SessionControlCard(
            state = state,
            onConnect = onConnect,
            onOpenSession = onOpenSession,
            onRefreshStatus = onRefreshStatus,
            onAttachTarget = onAttachTarget,
            onTargetPidChanged = onTargetPidChanged,
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
