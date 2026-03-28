package com.smlc666.lkmdbg.ui.screens

import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
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
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.data.MemoryPage
import com.smlc666.lkmdbg.data.MemoryRegionPreset
import com.smlc666.lkmdbg.data.MemorySearchResult
import com.smlc666.lkmdbg.data.MemorySearchValueType
import com.smlc666.lkmdbg.data.MemoryScalarValue
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
    onJumpMemoryAddress: () -> Unit,
    onStepMemoryPage: (Int) -> Unit,
    onSelectMemoryAddress: (ULong) -> Unit,
    onMemoryAddressChanged: (String) -> Unit,
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
                value = state.memoryAddressInput,
                onValueChange = onMemoryAddressChanged,
                modifier = Modifier.fillMaxWidth(),
                label = { Text(stringResource(R.string.memory_address_label)) },
                placeholder = { Text(stringResource(R.string.memory_address_placeholder)) },
                singleLine = true,
            )
            Spacer(Modifier.height(10.dp))

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilledTonalButton(onClick = onJumpMemoryAddress, enabled = !state.busy) {
                    Text(stringResource(R.string.memory_action_jump))
                }
                FilledTonalButton(
                    onClick = { onStepMemoryPage(-1) },
                    enabled = !state.busy && state.memoryPage != null,
                ) {
                    Text(stringResource(R.string.memory_action_prev_page))
                }
                FilledTonalButton(
                    onClick = { onStepMemoryPage(1) },
                    enabled = !state.busy && state.memoryPage != null,
                ) {
                    Text(stringResource(R.string.memory_action_next_page))
                }
            }

            Spacer(Modifier.height(8.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilledTonalButton(onClick = onPreviewSelectedPc, enabled = !state.busy) {
                    Text(stringResource(R.string.memory_action_preview_pc))
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
            }

            Spacer(Modifier.height(14.dp))
            state.memoryPage?.let { page ->
                MemoryPageCard(
                    page = page,
                    onSelectMemoryAddress = onSelectMemoryAddress,
                )
                Spacer(Modifier.height(14.dp))
            }

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

            FilledTonalButton(onClick = onSearchMemory, enabled = !state.busy) {
                Text(stringResource(R.string.memory_action_search))
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
                        onSelectMemoryAddress = onSelectMemoryAddress,
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
                                modifier = Modifier.clickable {
                                    onSelectMemoryAddress(vma.startAddr)
                                    showRanges = false
                                },
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
private fun MemoryPageCard(
    page: MemoryPage,
    onSelectMemoryAddress: (ULong) -> Unit,
) {
    Card(
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.55f),
        ),
    ) {
        Column(
            modifier = Modifier.padding(14.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Text(
                stringResource(R.string.memory_preview_title, hex64(page.pageStart)),
                style = MaterialTheme.typography.titleMedium,
            )
            Text(
                stringResource(R.string.memory_page_focus, hex64(page.focusAddress)),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.primary,
            )
            Text(
                stringResource(
                    R.string.memory_page_window,
                    page.pageSize.toString(),
                    page.bytes.size.toString(),
                ),
                style = MaterialTheme.typography.labelMedium,
            )
            Text(
                text = page.region?.let {
                    stringResource(
                        R.string.memory_page_region,
                        it.name.ifBlank { stringResource(R.string.memory_ranges_unnamed) },
                        hex64(it.startAddr),
                        hex64(it.endAddr),
                        it.prot.toString(),
                    )
                } ?: stringResource(R.string.memory_page_region_none),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )

            Text(
                stringResource(R.string.memory_page_scalars_title),
                style = MaterialTheme.typography.titleSmall,
            )
            ScalarRow(page.scalars)

            Text(
                stringResource(R.string.memory_preview_hex_title),
                style = MaterialTheme.typography.titleSmall,
            )
            page.rows.forEach { row ->
                val selected = page.focusAddress >= row.address && page.focusAddress < row.address + 16uL
                Card(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clickable { onSelectMemoryAddress(row.address) },
                    colors = CardDefaults.cardColors(
                        containerColor = if (selected) {
                            MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.65f)
                        } else {
                            MaterialTheme.colorScheme.surface.copy(alpha = 0.78f)
                        },
                    ),
                ) {
                    Text(
                        modifier = Modifier.padding(horizontal = 10.dp, vertical = 8.dp),
                        text = "${hex64(row.address)}  ${row.hexBytes.padEnd(47)}  ${row.ascii}",
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace,
                    )
                }
            }

            Text(
                stringResource(R.string.memory_preview_disasm_title),
                style = MaterialTheme.typography.titleSmall,
            )
            if (page.disassembly.isEmpty()) {
                Text(
                    stringResource(R.string.memory_preview_disasm_empty),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            } else {
                page.disassembly.forEach { line ->
                    Text(
                        text = line,
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace,
                    )
                }
            }
        }
    }
}

@Composable
private fun ScalarRow(scalars: List<MemoryScalarValue>) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .horizontalScroll(rememberScrollState()),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        scalars.forEach { scalar ->
            Card(
                colors = CardDefaults.cardColors(
                    containerColor = MaterialTheme.colorScheme.surface.copy(alpha = 0.82f),
                ),
            ) {
                Column(
                    modifier = Modifier.padding(horizontal = 12.dp, vertical = 10.dp),
                    verticalArrangement = Arrangement.spacedBy(4.dp),
                ) {
                    Text(
                        scalar.label,
                        style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.primary,
                    )
                    Text(
                        scalar.value,
                        style = MaterialTheme.typography.bodyMedium,
                        fontFamily = FontFamily.Monospace,
                    )
                }
            }
        }
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
    onSelectMemoryAddress: (ULong) -> Unit,
) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .clickable { onSelectMemoryAddress(result.address) },
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
