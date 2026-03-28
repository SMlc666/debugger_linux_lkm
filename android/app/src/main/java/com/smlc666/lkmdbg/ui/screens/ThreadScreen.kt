package com.smlc666.lkmdbg.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.weight
import androidx.compose.material3.AssistChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.ui.DashboardState
import com.smlc666.lkmdbg.ui.components.PanelCard

@Composable
internal fun ThreadScreen(state: DashboardState) {
    Column(verticalArrangement = Arrangement.spacedBy(14.dp)) {
        PanelCard(
            title = stringResource(R.string.thread_panel_title),
            subtitle = stringResource(R.string.thread_panel_subtitle),
        ) {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                state.threads.forEach { thread ->
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                    ) {
                        Column(Modifier.weight(1f)) {
                            Text("${thread.name} (${thread.tid})", style = MaterialTheme.typography.titleMedium)
                            Text(
                                stringResource(R.string.thread_pc_value, thread.pc),
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                        AssistChip(onClick = {}, label = { Text(thread.state) })
                    }
                }
            }
        }
    }
}
