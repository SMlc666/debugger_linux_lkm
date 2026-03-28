package com.smlc666.lkmdbg.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.weight
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
internal fun ProcessSummaryScreen(state: DashboardState) {
    PanelCard(
        title = stringResource(R.string.process_panel_title),
        subtitle = stringResource(R.string.process_panel_subtitle),
    ) {
        Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
            state.processes.forEach { process ->
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                ) {
                    Column(Modifier.weight(1f)) {
                        Text(process.name, style = MaterialTheme.typography.titleMedium)
                        Text(process.packageName, style = MaterialTheme.typography.bodyMedium)
                    }
                    Text(process.state, color = MaterialTheme.colorScheme.primary)
                }
            }
        }
    }
}
