package com.smlc666.lkmdbg.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.Spacer
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
internal fun MemoryScreen(state: DashboardState) {
    Column(verticalArrangement = Arrangement.spacedBy(14.dp)) {
        PanelCard(
            title = stringResource(R.string.memory_panel_title),
            subtitle = stringResource(R.string.memory_panel_subtitle),
        ) {
            Text(text = stringResource(R.string.memory_recent_scan, state.scanSummary), style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.height(12.dp))
            state.memoryRows.forEach { row ->
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                ) {
                    Text(row.label, style = MaterialTheme.typography.bodyLarge)
                    Text(row.value, style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.primary)
                }
            }
        }
    }
}
