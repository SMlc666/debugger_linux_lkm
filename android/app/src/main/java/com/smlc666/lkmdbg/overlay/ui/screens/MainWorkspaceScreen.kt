package com.smlc666.lkmdbg.overlay.ui.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.data.MemoryPage
import com.smlc666.lkmdbg.data.MemoryPreviewRow
import com.smlc666.lkmdbg.data.MemorySearchResult
import com.smlc666.lkmdbg.data.SessionBridgeState
import com.smlc666.lkmdbg.data.WorkspaceSection

private const val MEMORY_VIEW_MODE_PAGE = 0
private const val MEMORY_VIEW_MODE_RESULTS = 1

@Composable
fun MainWorkspaceScreen(
    state: SessionBridgeState,
    memoryViewMode: Int,
    memoryToolsOpen: Boolean,
    onSectionSelected: (WorkspaceSection) -> Unit,
    onToggleProcessPicker: () -> Unit,
    onStepMemoryPage: (Int) -> Unit,
    onSelectMemoryAddress: (ULong) -> Unit,
    onCycleMemorySearchValueType: () -> Unit,
    onCycleMemorySearchRefineMode: () -> Unit,
    onCycleMemoryRegionPreset: () -> Unit,
    onRunMemorySearch: () -> Unit,
    onRefineMemorySearch: () -> Unit,
    onShowMemoryResults: () -> Unit,
    onShowMemoryPage: () -> Unit,
    onPreviewSelectedPc: () -> Unit,
) {
    val configuration = androidx.compose.ui.platform.LocalConfiguration.current
    val isLandscape = configuration.orientation == android.content.res.Configuration.ORIENTATION_LANDSCAPE
    val sections = listOf(
        WorkspaceSection.Session,
        WorkspaceSection.Processes,
        WorkspaceSection.Memory,
        WorkspaceSection.Threads,
        WorkspaceSection.Events,
    )

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background),
    ) {
        if (isLandscape) {
            Row(modifier = Modifier.fillMaxSize()) {
                CategoryColumn(
                    sections = sections,
                    selectedSection = state.workspaceSection,
                    onSectionSelected = onSectionSelected,
                    modifier = Modifier
                        .width(80.dp)
                        .fillMaxHeight()
                )

                Column(
                    modifier = Modifier
                        .weight(1f)
                        .fillMaxHeight()
                ) {
                    Spacer(modifier = Modifier.height(48.dp).fillMaxWidth())

                    Box(
                        modifier = Modifier
                            .weight(1f)
                            .fillMaxWidth()
                            .padding(8.dp),
                    ) {
                        MainContentArea(
                            state = state,
                            memoryViewMode = memoryViewMode,
                            memoryToolsOpen = memoryToolsOpen,
                            onStepMemoryPage = onStepMemoryPage,
                            onSelectMemoryAddress = onSelectMemoryAddress,
                            onCycleMemorySearchValueType = onCycleMemorySearchValueType,
                            onCycleMemorySearchRefineMode = onCycleMemorySearchRefineMode,
                            onCycleMemoryRegionPreset = onCycleMemoryRegionPreset,
                            onRunMemorySearch = onRunMemorySearch,
                            onRefineMemorySearch = onRefineMemorySearch,
                            onShowMemoryResults = onShowMemoryResults,
                            onShowMemoryPage = onShowMemoryPage,
                            onPreviewSelectedPc = onPreviewSelectedPc,
                        )
                    }
                }
            }
        } else {
            Column(modifier = Modifier.fillMaxSize()) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    CategoryRow(
                        sections = sections,
                        selectedSection = state.workspaceSection,
                        onSectionSelected = onSectionSelected,
                        modifier = Modifier.weight(1f)
                    )
                    Spacer(modifier = Modifier.width(48.dp))
                }

                Box(
                    modifier = Modifier
                        .weight(1f)
                        .fillMaxWidth()
                        .padding(8.dp),
                ) {
                    MainContentArea(
                        state = state,
                        memoryViewMode = memoryViewMode,
                        memoryToolsOpen = memoryToolsOpen,
                        onStepMemoryPage = onStepMemoryPage,
                        onSelectMemoryAddress = onSelectMemoryAddress,
                        onCycleMemorySearchValueType = onCycleMemorySearchValueType,
                        onCycleMemorySearchRefineMode = onCycleMemorySearchRefineMode,
                        onCycleMemoryRegionPreset = onCycleMemoryRegionPreset,
                        onRunMemorySearch = onRunMemorySearch,
                        onRefineMemorySearch = onRefineMemorySearch,
                        onShowMemoryResults = onShowMemoryResults,
                        onShowMemoryPage = onShowMemoryPage,
                        onPreviewSelectedPc = onPreviewSelectedPc,
                    )
                }
            }
        }

        Box(
            modifier = Modifier.fillMaxSize(),
            contentAlignment = Alignment.TopEnd
        ) {
            AppListButton(onClick = onToggleProcessPicker)
        }
    }
}

@Composable
fun CategoryColumn(
    sections: List<WorkspaceSection>,
    selectedSection: WorkspaceSection,
    onSectionSelected: (WorkspaceSection) -> Unit,
    modifier: Modifier
) {
    Column(
        modifier = modifier.background(MaterialTheme.colorScheme.surfaceVariant)
    ) {
        sections.forEach { section ->
            val isSelected = selectedSection == section
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .weight(1f)
                    .background(
                        if (isSelected) {
                            MaterialTheme.colorScheme.primaryContainer
                        } else {
                            Color.Transparent
                        },
                    )
                    .clickable { onSectionSelected(section) },
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    text = section.name,
                    color = if (isSelected) {
                        MaterialTheme.colorScheme.onPrimaryContainer
                    } else {
                        MaterialTheme.colorScheme.onSurfaceVariant
                    },
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }
    }
}

@Composable
fun CategoryRow(
    sections: List<WorkspaceSection>,
    selectedSection: WorkspaceSection,
    onSectionSelected: (WorkspaceSection) -> Unit,
    modifier: Modifier
) {
    Row(
        modifier = modifier
            .background(MaterialTheme.colorScheme.surfaceVariant)
            .height(48.dp)
    ) {
        sections.forEach { section ->
            val isSelected = selectedSection == section
            Box(
                modifier = Modifier
                    .weight(1f)
                    .fillMaxHeight()
                    .background(
                        if (isSelected) {
                            MaterialTheme.colorScheme.primaryContainer
                        } else {
                            Color.Transparent
                        },
                    )
                    .clickable { onSectionSelected(section) },
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    text = section.name,
                    color = if (isSelected) {
                        MaterialTheme.colorScheme.onPrimaryContainer
                    } else {
                        MaterialTheme.colorScheme.onSurfaceVariant
                    },
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }
    }
}

@Composable
fun AppListButton(onClick: () -> Unit) {
    androidx.compose.material3.IconButton(onClick = onClick) {
        Text("📱")
    }
}

@Composable
fun MainContentArea(
    state: SessionBridgeState,
    memoryViewMode: Int,
    memoryToolsOpen: Boolean,
    onStepMemoryPage: (Int) -> Unit,
    onSelectMemoryAddress: (ULong) -> Unit,
    onCycleMemorySearchValueType: () -> Unit,
    onCycleMemorySearchRefineMode: () -> Unit,
    onCycleMemoryRegionPreset: () -> Unit,
    onRunMemorySearch: () -> Unit,
    onRefineMemorySearch: () -> Unit,
    onShowMemoryResults: () -> Unit,
    onShowMemoryPage: () -> Unit,
    onPreviewSelectedPc: () -> Unit,
) {
    when (state.workspaceSection) {
        WorkspaceSection.Session -> {
            Text(
                text = "Session Overview (PID: ${state.snapshot.targetPid})",
                color = MaterialTheme.colorScheme.onBackground,
            )
        }

        WorkspaceSection.Processes -> {
            Text(
                text = "Select a process from the picker above.",
                color = MaterialTheme.colorScheme.onBackground,
            )
        }

        WorkspaceSection.Memory -> {
            MemorySectionContent(
                state = state,
                memoryToolsOpen = memoryToolsOpen,
                viewMode = memoryViewMode,
                onStepPage = onStepMemoryPage,
                onSelectAddress = onSelectMemoryAddress,
                onCycleValueType = onCycleMemorySearchValueType,
                onCycleRefineMode = onCycleMemorySearchRefineMode,
                onCycleRegionPreset = onCycleMemoryRegionPreset,
                onRunSearch = onRunMemorySearch,
                onRefineSearch = onRefineMemorySearch,
                onShowResults = onShowMemoryResults,
                onShowPage = onShowMemoryPage,
                onPreviewSelectedPc = onPreviewSelectedPc,
            )
        }

        WorkspaceSection.Threads -> {
            Text(
                text = "Threads not implemented yet.",
                color = MaterialTheme.colorScheme.onBackground,
            )
        }

        WorkspaceSection.Events -> {
            Text(
                text = "Events not implemented yet.",
                color = MaterialTheme.colorScheme.onBackground,
            )
        }
    }
}

@Composable
private fun MemorySectionContent(
    state: SessionBridgeState,
    memoryToolsOpen: Boolean,
    viewMode: Int,
    onStepPage: (Int) -> Unit,
    onSelectAddress: (ULong) -> Unit,
    onCycleValueType: () -> Unit,
    onCycleRefineMode: () -> Unit,
    onCycleRegionPreset: () -> Unit,
    onRunSearch: () -> Unit,
    onRefineSearch: () -> Unit,
    onShowResults: () -> Unit,
    onShowPage: () -> Unit,
    onPreviewSelectedPc: () -> Unit,
) {
    val valueTypeLabel = stringResource(state.memorySearch.valueType.labelRes)
    val refineModeLabel = stringResource(state.memorySearch.refineMode.labelRes)
    val regionPresetLabel = stringResource(state.memorySearch.regionPreset.labelRes)
    val toolsLabel = stringResource(R.string.memory_action_tools)

    Column(modifier = Modifier.fillMaxSize()) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = stringResource(R.string.workspace_memory),
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.onBackground,
                )
                Text(
                    text = buildString {
                        append(valueTypeLabel)
                        append(" · ")
                        append(refineModeLabel)
                        append(" · ")
                        append(regionPresetLabel)
                        if (memoryToolsOpen)
                            append(" · $toolsLabel")
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            if (viewMode == MEMORY_VIEW_MODE_RESULTS) {
                OutlinedButton(onClick = onShowPage) {
                    Text(stringResource(R.string.memory_action_show_page))
                }
            } else if (state.memorySearch.results.isNotEmpty()) {
                OutlinedButton(onClick = onShowResults) {
                    Text(
                        stringResource(
                            R.string.memory_action_show_results_count,
                            state.memorySearch.results.size,
                        ),
                    )
                }
            }
        }
        Spacer(modifier = Modifier.height(8.dp))
        Row(modifier = Modifier.fillMaxWidth()) {
            OutlinedButton(onClick = onCycleValueType, modifier = Modifier.weight(1f)) {
                Text(valueTypeLabel)
            }
            Spacer(modifier = Modifier.width(8.dp))
            OutlinedButton(onClick = onCycleRefineMode, modifier = Modifier.weight(1f)) {
                Text(refineModeLabel)
            }
            Spacer(modifier = Modifier.width(8.dp))
            OutlinedButton(onClick = onCycleRegionPreset, modifier = Modifier.weight(1f)) {
                Text(regionPresetLabel)
            }
        }
        Spacer(modifier = Modifier.height(8.dp))
        Row(modifier = Modifier.fillMaxWidth()) {
            Button(onClick = onRunSearch, modifier = Modifier.weight(1f)) {
                Text(stringResource(R.string.memory_action_search))
            }
            Spacer(modifier = Modifier.width(8.dp))
            OutlinedButton(onClick = onRefineSearch, modifier = Modifier.weight(1f)) {
                Text(stringResource(R.string.memory_action_refine))
            }
            Spacer(modifier = Modifier.width(8.dp))
            OutlinedButton(onClick = onPreviewSelectedPc, modifier = Modifier.weight(1f)) {
                Text(stringResource(R.string.memory_action_preview_pc))
            }
        }
        if (viewMode == MEMORY_VIEW_MODE_PAGE) {
            Spacer(modifier = Modifier.height(8.dp))
            Row(modifier = Modifier.fillMaxWidth()) {
                OutlinedButton(onClick = { onStepPage(-1) }, modifier = Modifier.weight(1f)) {
                    Text(stringResource(R.string.memory_action_prev_page))
                }
                Spacer(modifier = Modifier.width(8.dp))
                OutlinedButton(onClick = { onStepPage(1) }, modifier = Modifier.weight(1f)) {
                    Text(stringResource(R.string.memory_action_next_page))
                }
            }
        }
        Spacer(modifier = Modifier.height(12.dp))
        Surface(
            modifier = Modifier
                .weight(1f)
                .fillMaxWidth(),
            color = MaterialTheme.colorScheme.surface,
            tonalElevation = 2.dp,
        ) {
            if (viewMode == MEMORY_VIEW_MODE_RESULTS) {
                MemoryResultsList(
                    state = state,
                    onSelectAddress = onSelectAddress,
                )
            } else {
                MemoryPageList(
                    page = state.memoryPage,
                    onSelectAddress = onSelectAddress,
                )
            }
        }
    }
}

@Composable
private fun MemoryResultsList(
    state: SessionBridgeState,
    onSelectAddress: (ULong) -> Unit,
) {
    val results = state.memorySearch.results
    if (results.isEmpty()) {
        Box(
            modifier = Modifier.fillMaxSize(),
            contentAlignment = Alignment.Center,
        ) {
            Text(
                text = stringResource(R.string.memory_search_results_empty),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        return
    }
    LazyColumn(modifier = Modifier.fillMaxSize()) {
        items(results, key = { it.address.toString() }) { result ->
            MemoryResultCard(
                result = result,
                selected = state.memoryPage?.focusAddress == result.address,
                onClick = { onSelectAddress(result.address) },
            )
        }
    }
}

@Composable
private fun MemoryResultCard(
    result: MemorySearchResult,
    selected: Boolean,
    onClick: () -> Unit,
) {
    val unnamedRangeLabel = stringResource(R.string.memory_ranges_unnamed)

    Surface(
        modifier = Modifier
            .fillMaxWidth()
            .padding(8.dp)
            .clickable(onClick = onClick),
        color = if (selected) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant,
        tonalElevation = 1.dp,
    ) {
        Column(modifier = Modifier.padding(12.dp)) {
            Text(
                text = "${hex64(result.address)} · ${result.valueSummary}",
                style = MaterialTheme.typography.bodyMedium,
                color = if (selected) {
                    MaterialTheme.colorScheme.onPrimaryContainer
                } else {
                    MaterialTheme.colorScheme.onSurfaceVariant
                },
            )
            Spacer(modifier = Modifier.height(4.dp))
            Text(
                text = buildString {
                    append(result.regionName.ifBlank { unnamedRangeLabel })
                    append(" · ")
                    append(hex64(result.regionStart))
                    append(" - ")
                    append(hex64(result.regionEnd))
                },
                style = MaterialTheme.typography.bodySmall,
                color = if (selected) {
                    MaterialTheme.colorScheme.onPrimaryContainer
                } else {
                    MaterialTheme.colorScheme.onSurfaceVariant
                },
            )
            if (result.previewHex.isNotBlank()) {
                Spacer(modifier = Modifier.height(4.dp))
                Text(
                    text = result.previewHex.take(32),
                    style = MaterialTheme.typography.labelSmall,
                    color = if (selected) {
                        MaterialTheme.colorScheme.onPrimaryContainer
                    } else {
                        MaterialTheme.colorScheme.onSurfaceVariant
                    },
                )
            }
        }
    }
}

@Composable
private fun MemoryPageList(
    page: MemoryPage?,
    onSelectAddress: (ULong) -> Unit,
) {
    if (page == null) {
        Box(
            modifier = Modifier.fillMaxSize(),
            contentAlignment = Alignment.Center,
        ) {
            Text(
                text = stringResource(R.string.memory_error_no_page),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        return
    }
    val regionSummary = page.region?.let { region ->
        "${region.name}  ${hex64(region.startAddr)} - ${hex64(region.endAddr)}  prot=${region.prot.toString(16)}"
    } ?: stringResource(R.string.memory_page_region_none)

    Column(modifier = Modifier.fillMaxSize()) {
        Column(modifier = Modifier.padding(12.dp)) {
            Text(
                text = stringResource(R.string.memory_page_focus, hex64(page.focusAddress)),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurface,
            )
            Text(
                text = stringResource(R.string.memory_page_window, page.pageSize.toInt(), page.bytes.size),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text(
                text = regionSummary,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            if (page.scalars.isNotEmpty()) {
                Spacer(modifier = Modifier.height(8.dp))
                Text(
                    text = stringResource(R.string.memory_page_scalars_title),
                    style = MaterialTheme.typography.labelLarge,
                    color = MaterialTheme.colorScheme.onSurface,
                )
                page.scalars.take(4).forEach { scalar ->
                    Text(
                        text = "${scalar.label}: ${scalar.value}",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
        HorizontalDivider()
        LazyColumn(modifier = Modifier.fillMaxSize()) {
            items(page.rows, key = { it.address.toString() }) { row ->
                MemoryPageRowCard(
                    row = row,
                    selected = rowContainsAddress(row, page.focusAddress),
                    onClick = { onSelectAddress(row.address) },
                )
            }
        }
    }
}

@Composable
private fun MemoryPageRowCard(
    row: MemoryPreviewRow,
    selected: Boolean,
    onClick: () -> Unit,
) {
    Surface(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 8.dp, vertical = 4.dp)
            .clickable(onClick = onClick),
        color = if (selected) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant,
        tonalElevation = 1.dp,
    ) {
        Column(modifier = Modifier.padding(12.dp)) {
            Text(
                text = "${hex64(row.address)} · ${row.hexBytes}",
                style = MaterialTheme.typography.bodyMedium,
                color = if (selected) {
                    MaterialTheme.colorScheme.onPrimaryContainer
                } else {
                    MaterialTheme.colorScheme.onSurfaceVariant
                },
            )
            Spacer(modifier = Modifier.height(4.dp))
            Text(
                text = row.ascii,
                style = MaterialTheme.typography.bodySmall,
                color = if (selected) {
                    MaterialTheme.colorScheme.onPrimaryContainer
                } else {
                    MaterialTheme.colorScheme.onSurfaceVariant
                },
            )
        }
    }
}

private fun rowContainsAddress(
    row: MemoryPreviewRow,
    address: ULong,
): Boolean {
    val rowEndExclusive = row.address + row.byteValues.size.toUInt().toULong()
    return address >= row.address && address < rowEndExclusive
}

private fun hex64(value: ULong): String = "0x${value.toString(16)}"
