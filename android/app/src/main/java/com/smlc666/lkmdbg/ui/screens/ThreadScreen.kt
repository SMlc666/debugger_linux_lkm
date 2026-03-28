package com.smlc666.lkmdbg.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.clickable
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
internal fun ThreadScreen(
    state: SessionBridgeState,
    onRefreshThreads: () -> Unit,
    onSelectThread: (Int) -> Unit,
) {
    LaunchedEffect(state.snapshot.targetPid, state.threads.isEmpty()) {
        if (state.snapshot.targetPid > 0 && state.threads.isEmpty())
            onRefreshThreads()
    }

    Column(verticalArrangement = Arrangement.spacedBy(14.dp)) {
        PanelCard(
            title = stringResource(R.string.thread_panel_title),
            subtitle = stringResource(R.string.thread_panel_subtitle),
        ) {
            FilledTonalButton(onClick = onRefreshThreads, enabled = !state.busy) {
                Text(stringResource(R.string.thread_action_refresh))
            }
            Spacer(Modifier.height(12.dp))
            if (state.threads.isEmpty()) {
                Text(stringResource(R.string.thread_empty))
                return@PanelCard
            }
            state.selectedThreadRegisters?.let { regs ->
                Card(
                    colors = CardDefaults.cardColors(
                        containerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.55f),
                    ),
                ) {
                    Column(modifier = Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        Text(
                            stringResource(R.string.thread_regs_title, regs.tid),
                            style = MaterialTheme.typography.titleMedium,
                        )
                        Text(stringResource(R.string.thread_pc_value, hex64(regs.pc)))
                        Text(stringResource(R.string.thread_sp_value, hex64(regs.sp)))
                        Text(stringResource(R.string.thread_pstate_value, hex64(regs.pstate)))
                        Text(stringResource(R.string.thread_reg_x0, hex64(regs.regs[0])))
                        Text(stringResource(R.string.thread_reg_x1, hex64(regs.regs[1])))
                        Text(stringResource(R.string.thread_reg_x29, hex64(regs.regs[29])))
                        Text(stringResource(R.string.thread_reg_x30, hex64(regs.regs[30])))
                        Text(
                            stringResource(
                                R.string.thread_fp_summary,
                                regs.features.toString(),
                                regs.fpsr.toString(),
                                regs.fpcr.toString(),
                                hex64(regs.v0Lo),
                                hex64(regs.v0Hi),
                            ),
                        )
                    }
                }
                Spacer(Modifier.height(12.dp))
            }
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                state.threads.forEach { thread ->
                    Card(
                        colors = CardDefaults.cardColors(
                            containerColor = MaterialTheme.colorScheme.surface.copy(alpha = 0.82f),
                        ),
                        modifier = Modifier
                            .fillMaxWidth()
                            .clickable { onSelectThread(thread.tid) },
                    ) {
                        Column(
                            modifier = Modifier.padding(14.dp),
                            verticalArrangement = Arrangement.spacedBy(6.dp),
                        ) {
                            Text("${thread.comm} (${thread.tid})", style = MaterialTheme.typography.titleMedium)
                            Text(
                                stringResource(R.string.thread_group_value, thread.tgid),
                                style = MaterialTheme.typography.bodyMedium,
                            )
                            Text(
                                stringResource(R.string.thread_pc_value, hex64(thread.userPc)),
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                            Text(
                                stringResource(R.string.thread_sp_value, hex64(thread.userSp)),
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                            Text(
                                stringResource(R.string.thread_flags_value, threadFlagsLabel(thread.flags)),
                                style = MaterialTheme.typography.labelMedium,
                                color = MaterialTheme.colorScheme.primary,
                            )
                        }
                    }
                }
            }
        }
    }
}

private fun hex64(value: ULong): String = "0x${value.toString(16)}"

private fun threadFlagsLabel(flags: UInt): String = buildList {
    if ((flags and 0x00000001u) != 0u) add("leader")
    if ((flags and 0x00000002u) != 0u) add("target")
    if ((flags and 0x00000004u) != 0u) add("tracked")
    if ((flags and 0x00000008u) != 0u) add("settled")
    if ((flags and 0x00000010u) != 0u) add("parked")
    if ((flags and 0x00000020u) != 0u) add("exiting")
}.ifEmpty { listOf("none") }.joinToString(", ")
