package com.smlc666.lkmdbg.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.padding
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
import com.smlc666.lkmdbg.ui.DashboardState
import com.smlc666.lkmdbg.ui.components.PanelCard

@Composable
internal fun MemoryScreen(
    dashboardState: DashboardState,
    state: SessionBridgeState,
    onRefreshImages: () -> Unit,
    onPreviewSelectedPc: () -> Unit,
) {
    LaunchedEffect(state.snapshot.targetPid, state.images.isEmpty()) {
        if (state.snapshot.targetPid > 0 && state.images.isEmpty())
            onRefreshImages()
    }

    Column(verticalArrangement = Arrangement.spacedBy(14.dp)) {
        PanelCard(
            title = stringResource(R.string.memory_panel_title),
            subtitle = stringResource(R.string.memory_panel_subtitle),
        ) {
            Text(
                text = stringResource(R.string.memory_recent_scan, dashboardState.scanSummary),
                style = MaterialTheme.typography.titleMedium,
            )
            Spacer(Modifier.height(12.dp))
            FilledTonalButton(onClick = onRefreshImages, enabled = !state.busy) {
                Text(stringResource(R.string.memory_action_refresh_images))
            }
            Spacer(Modifier.height(8.dp))
            FilledTonalButton(onClick = onPreviewSelectedPc, enabled = !state.busy) {
                Text(stringResource(R.string.memory_action_preview_pc))
            }
            Spacer(Modifier.height(12.dp))
            dashboardState.memoryRows.forEach { row ->
                androidx.compose.foundation.layout.Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                ) {
                    Text(row.label, style = MaterialTheme.typography.bodyLarge)
                    Text(
                        row.value,
                        style = MaterialTheme.typography.bodyLarge,
                        color = MaterialTheme.colorScheme.primary,
                    )
                }
            }
            Spacer(Modifier.height(14.dp))
            Text(
                stringResource(R.string.memory_images_count, state.images.size),
                style = MaterialTheme.typography.titleMedium,
            )
            Spacer(Modifier.height(8.dp))
            if (state.images.isEmpty()) {
                Text(stringResource(R.string.memory_images_empty))
            } else {
                state.images.take(6).forEach { image ->
                    Card(
                        colors = CardDefaults.cardColors(
                            containerColor = MaterialTheme.colorScheme.surface.copy(alpha = 0.82f),
                        ),
                    ) {
                        Column(
                            modifier = Modifier.padding(14.dp),
                            verticalArrangement = Arrangement.spacedBy(4.dp),
                        ) {
                            Text(image.name, style = MaterialTheme.typography.titleMedium)
                            Text(
                                stringResource(
                                    R.string.memory_image_range,
                                    hex64(image.startAddr),
                                    hex64(image.endAddr),
                                    hex64(image.baseAddr),
                                ),
                            )
                            Text(
                                stringResource(
                                    R.string.memory_image_flags,
                                    image.prot.toString(),
                                    image.flags.toString(),
                                    image.segmentCount.toString(),
                                ),
                                style = MaterialTheme.typography.bodyMedium,
                            )
                        }
                    }
                    Spacer(Modifier.height(8.dp))
                }
            }
            state.memoryPreview?.let { preview ->
                Spacer(Modifier.height(10.dp))
                Card(
                    colors = CardDefaults.cardColors(
                        containerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.55f),
                    ),
                ) {
                    Column(
                        modifier = Modifier.padding(14.dp),
                        verticalArrangement = Arrangement.spacedBy(6.dp),
                    ) {
                        Text(
                            stringResource(R.string.memory_preview_title, hex64(preview.address)),
                            style = MaterialTheme.typography.titleMedium,
                        )
                        Text(
                            stringResource(R.string.memory_preview_bytes, preview.bytes.size),
                            style = MaterialTheme.typography.labelMedium,
                            color = MaterialTheme.colorScheme.primary,
                        )
                        Text(
                            preview.bytes.joinToString(" ") { "%02x".format(it.toInt() and 0xff) },
                            style = MaterialTheme.typography.bodyMedium,
                        )
                    }
                }
            }
        }
    }
}

private fun hex64(value: ULong): String = "0x${value.toString(16)}"
