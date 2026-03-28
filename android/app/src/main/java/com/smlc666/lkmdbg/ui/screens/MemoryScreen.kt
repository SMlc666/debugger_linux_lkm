package com.smlc666.lkmdbg.ui.screens

import androidx.compose.foundation.clickable
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
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.data.MemoryRegionPreset
import com.smlc666.lkmdbg.data.MemorySearchResult
import com.smlc666.lkmdbg.data.MemorySearchValueType
import com.smlc666.lkmdbg.data.SessionBridgeState
import com.smlc666.lkmdbg.ui.DashboardState
import com.smlc666.lkmdbg.ui.components.PanelCard

@Composable
internal fun MemoryScreen(
    dashboardState: DashboardState,
    state: SessionBridgeState,
    onRefreshImages: () -> Unit,
    onRefreshVmas: () -> Unit,
    onSearchMemory: () -> Unit,
    onPreviewSelectedPc: () -> Unit,
    onPreviewAddress: (ULong) -> Unit,
    onSearchQueryChanged: (String) -> Unit,
    onSearchValueTypeChanged: (MemorySearchValueType) -> Unit,
    onRegionPresetChanged: (MemoryRegionPreset) -> Unit,
) {
    var showRanges by rememberSaveable { mutableStateOf(false) }

    LaunchedEffect(state.snapshot.targetPid, state.images.isEmpty(), state.vmas.isEmpty()) {
        if (state.snapshot.targetPid > 0) {
            if (state.images.isEmpty())
                onRefreshImages()
            if (state.vmas.isEmpty())
                onRefreshVmas()
        }
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

            OutlinedTextField(
                value = state.memorySearch.query,
                onValueChange = onSearchQueryChanged,
                modifier = Modifier.fillMaxWidth(),
                label = { Text(stringResource(R.string.memory_search_query_label)) },
                placeholder = { Text(stringResource(R.string.memory_search_query_placeholder)) },
                singleLine = true,
            )
            Spacer(Modifier.height(10.dp))

            MemoryChipRow {
                MemorySearchValueType.entries.forEach { type ->
                    FilterChip(
                        selected = state.memorySearch.valueType == type,
                        onClick = { onSearchValueTypeChanged(type) },
                        label = { Text(stringResource(type.labelRes)) },
                    )
                }
            }
            Spacer(Modifier.height(10.dp))
            MemoryChipRow {
                MemoryRegionPreset.entries.forEach { preset ->
                    FilterChip(
                        selected = state.memorySearch.regionPreset == preset,
                        onClick = { onRegionPresetChanged(preset) },
                        label = { Text(stringResource(preset.labelRes)) },
                    )
                }
            }
            Spacer(Modifier.height(12.dp))

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilledTonalButton(onClick = onSearchMemory, enabled = !state.busy) {
                    Text(stringResource(R.string.memory_action_search))
                }
                FilledTonalButton(onClick = onRefreshVmas, enabled = !state.busy) {
                    Text(stringResource(R.string.memory_action_refresh_ranges))
                }
                FilledTonalButton(onClick = { showRanges = true }, enabled = state.vmas.isNotEmpty()) {
                    Text(stringResource(R.string.memory_action_ranges))
                }
            }

            Spacer(Modifier.height(8.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilledTonalButton(onClick = onRefreshImages, enabled = !state.busy) {
                    Text(stringResource(R.string.memory_action_refresh_images))
                }
                FilledTonalButton(onClick = onPreviewSelectedPc, enabled = !state.busy) {
                    Text(stringResource(R.string.memory_action_preview_pc))
                }
            }

            Spacer(Modifier.height(12.dp))
            dashboardState.memoryRows.forEach { row ->
                Row(
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
                text = stringResource(R.string.memory_ranges_count, state.vmas.size),
                style = MaterialTheme.typography.titleMedium,
            )
            if (state.memorySearch.summary.isNotBlank()) {
                Spacer(Modifier.height(6.dp))
                Text(
                    state.memorySearch.summary,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.primary,
                )
            }
            Spacer(Modifier.height(10.dp))

            if (state.memorySearch.results.isEmpty()) {
                Text(stringResource(R.string.memory_search_results_empty))
            } else {
                state.memorySearch.results.take(24).forEach { result ->
                    MemorySearchResultCard(
                        result = result,
                        onPreviewAddress = onPreviewAddress,
                    )
                    Spacer(Modifier.height(8.dp))
                }
            }

            Spacer(Modifier.height(10.dp))
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

    if (showRanges) {
        AlertDialog(
            onDismissRequest = { showRanges = false },
            confirmButton = {
                TextButton(onClick = { showRanges = false }) {
                    Text(stringResource(R.string.memory_ranges_close))
                }
            },
            title = { Text(stringResource(R.string.memory_ranges_dialog_title)) },
            text = {
                if (state.vmas.isEmpty()) {
                    Text(stringResource(R.string.memory_ranges_empty))
                } else {
                    LazyColumn(
                        modifier = Modifier.height(360.dp),
                        verticalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        items(state.vmas.take(48), key = { "${it.startAddr}:${it.endAddr}:${it.name}" }) { vma ->
                            Card(
                                colors = CardDefaults.cardColors(
                                    containerColor = MaterialTheme.colorScheme.surface.copy(alpha = 0.82f),
                                ),
                            ) {
                                Column(
                                    modifier = Modifier.padding(12.dp),
                                    verticalArrangement = Arrangement.spacedBy(4.dp),
                                ) {
                                    Text(vma.name.ifBlank { stringResource(R.string.memory_ranges_unnamed) })
                                    Text(
                                        stringResource(
                                            R.string.memory_ranges_item_range,
                                            hex64(vma.startAddr),
                                            hex64(vma.endAddr),
                                        ),
                                        style = MaterialTheme.typography.bodyMedium,
                                    )
                                    Text(
                                        stringResource(
                                            R.string.memory_ranges_item_flags,
                                            vma.prot.toString(),
                                            vma.flags.toString(),
                                        ),
                                        style = MaterialTheme.typography.labelMedium,
                                        color = MaterialTheme.colorScheme.primary,
                                    )
                                }
                            }
                        }
                    }
                }
            },
        )
    }
}

@Composable
private fun MemoryChipRow(content: @Composable () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .horizontalScroll(rememberScrollState()),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        content()
    }
}

@Composable
private fun MemorySearchResultCard(
    result: MemorySearchResult,
    onPreviewAddress: (ULong) -> Unit,
) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .clickable { onPreviewAddress(result.address) },
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surface.copy(alpha = 0.82f),
        ),
    ) {
        Column(
            modifier = Modifier.padding(14.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            Text(
                text = hex64(result.address),
                style = MaterialTheme.typography.titleMedium,
                color = MaterialTheme.colorScheme.primary,
            )
            Text(
                text = result.valueSummary,
                style = MaterialTheme.typography.bodyLarge,
            )
            Text(
                text = result.previewHex,
                style = MaterialTheme.typography.bodyMedium,
            )
            Text(
                text = "${result.regionName}  ${hex64(result.regionStart)} - ${hex64(result.regionEnd)}",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

private fun hex64(value: ULong): String = "0x${value.toString(16)}"
