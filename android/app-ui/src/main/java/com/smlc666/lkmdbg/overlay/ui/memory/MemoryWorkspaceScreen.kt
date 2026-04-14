package com.smlc666.lkmdbg.overlay.ui.memory

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Tab
import androidx.compose.material3.TabRow
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.unit.dp
import com.smlc666.lkmdbg.data.MemoryPreviewRow
import com.smlc666.lkmdbg.data.MemoryPage
import com.smlc666.lkmdbg.data.MemorySearchResult
import com.smlc666.lkmdbg.data.SessionBridgeState
import com.smlc666.lkmdbg.overlay.presentation.memory.MemoryTab
import com.smlc666.lkmdbg.overlay.presentation.memory.MemoryWorkspaceIntent
import com.smlc666.lkmdbg.overlay.presentation.memory.MemoryWorkspaceState
import com.smlc666.lkmdbg.overlay.presentation.memory.MemoryWorkspaceViewModel

private data class MenuTarget(
    val tab: MemoryTab,
    val address: ULong,
)

@Composable
fun MemoryWorkspaceSection(
    bridgeState: SessionBridgeState,
    onSearchQueryChanged: (String) -> Unit,
    onRunSearch: () -> Unit,
    onRefineSearch: () -> Unit,
    onCycleValueType: () -> Unit,
    onCycleRefineMode: () -> Unit,
    onCycleRegionPreset: () -> Unit,
    onStepPage: (Int) -> Unit,
    onSelectAddress: (ULong) -> Unit,
    onWriteHexAtAddress: ((ULong, String) -> Unit)? = null,
    onWriteHexAtAddresses: ((Set<ULong>, String) -> Unit)? = null,
    modifier: Modifier = Modifier,
) {
    // Session-local only: clear the saved list whenever a new session starts.
    val viewModel = remember(bridgeState.snapshot.sessionOpen, bridgeState.snapshot.sessionId) {
        MemoryWorkspaceViewModel(MemoryWorkspaceState.initial())
    }
    val workspaceState by viewModel.state.collectAsState()

    // External entry points (events, process attach, etc) drive focus changes via SessionBridgeState.
    // When focus changes to a real address, follow it into the Page tab.
    LaunchedEffect(bridgeState.memoryPage?.focusAddress) {
        if (bridgeState.memoryPage != null) {
            viewModel.dispatch(MemoryWorkspaceIntent.SwitchTab(MemoryTab.Page))
        }
    }

    MemoryWorkspaceScreen(
        bridgeState = bridgeState,
        workspaceState = workspaceState,
        dispatch = viewModel::dispatch,
        onSearchQueryChanged = onSearchQueryChanged,
        onRunSearch = onRunSearch,
        onRefineSearch = onRefineSearch,
        onCycleValueType = onCycleValueType,
        onCycleRefineMode = onCycleRefineMode,
        onCycleRegionPreset = onCycleRegionPreset,
        onStepPage = onStepPage,
        onSelectAddress = onSelectAddress,
        onWriteHexAtAddress = onWriteHexAtAddress,
        onWriteHexAtAddresses = onWriteHexAtAddresses,
        modifier = modifier,
    )
}

@Composable
fun MemoryWorkspaceScreen(
    bridgeState: SessionBridgeState,
    workspaceState: MemoryWorkspaceState,
    dispatch: (MemoryWorkspaceIntent) -> Unit,
    onSearchQueryChanged: (String) -> Unit,
    onRunSearch: () -> Unit,
    onRefineSearch: () -> Unit,
    onCycleValueType: () -> Unit,
    onCycleRefineMode: () -> Unit,
    onCycleRegionPreset: () -> Unit,
    onStepPage: (Int) -> Unit,
    onSelectAddress: (ULong) -> Unit,
    onWriteHexAtAddress: ((ULong, String) -> Unit)? = null,
    onWriteHexAtAddresses: ((Set<ULong>, String) -> Unit)? = null,
    modifier: Modifier = Modifier,
) {
    val clipboard = LocalClipboardManager.current

    var menuTarget by remember { mutableStateOf<MenuTarget?>(null) }
    var modifyTargets by remember { mutableStateOf<Set<ULong>?>(null) }
    val canModify = onWriteHexAtAddress != null || onWriteHexAtAddresses != null

    val searchRows = remember(bridgeState.memorySearch.results) {
        bridgeState.memorySearch.results.map { it.toRow() }
    }
    val savedRows = remember(workspaceState.saved.entries) {
        workspaceState.saved.entries.values.map { saved ->
            MemoryDataRow(
                address = saved.address,
                title = saved.label.ifBlank { "Saved" },
            )
        }
    }
    val pageRows = remember(bridgeState.memoryPage?.rows) {
        bridgeState.memoryPage?.rows?.map { it.toRow() } ?: emptyList()
    }

    Column(
        modifier = modifier.fillMaxSize(),
    ) {
        TabRow(
            selectedTabIndex = workspaceState.activeTab.ordinal,
        ) {
            MemoryTab.entries.forEach { tab ->
                Tab(
                    selected = tab == workspaceState.activeTab,
                    onClick = { dispatch(MemoryWorkspaceIntent.SwitchTab(tab)) },
                    text = { Text(tab.name) },
                )
            }
        }

        when (workspaceState.activeTab) {
            MemoryTab.Search -> {
                SearchControls(
                    bridgeState = bridgeState,
                    onSearchQueryChanged = onSearchQueryChanged,
                    onRunSearch = onRunSearch,
                    onRefineSearch = onRefineSearch,
                    onCycleValueType = onCycleValueType,
                    onCycleRefineMode = onCycleRefineMode,
                    onCycleRegionPreset = onCycleRegionPreset,
                )

                MemoryDataView(
                    rows = searchRows,
                    selectedAddresses = workspaceState.search.selection,
                    filterText = workspaceState.search.filterText,
                    onFilterTextChanged = { text ->
                        dispatch(MemoryWorkspaceIntent.SetFilter(MemoryTab.Search, text))
                    },
                    onToggleSelected = { address ->
                        dispatch(MemoryWorkspaceIntent.ToggleSelected(MemoryTab.Search, address))
                    },
                    onOpenMenu = { address ->
                        menuTarget = MenuTarget(MemoryTab.Search, address)
                    },
                    onClearSelection = {
                        dispatch(MemoryWorkspaceIntent.ClearSelection(MemoryTab.Search))
                    },
                    onAddSelectionToSaved = {
                        dispatch(MemoryWorkspaceIntent.AddToSaved(workspaceState.search.selection))
                    },
                    onModifySelection = if (!canModify) null else ({ addresses ->
                        modifyTargets = addresses
                    }),
                    modifier = Modifier.weight(1f),
                )
            }

            MemoryTab.Saved -> {
                MemoryDataView(
                    rows = savedRows,
                    selectedAddresses = workspaceState.saved.selection,
                    filterText = workspaceState.saved.filterText,
                    onFilterTextChanged = { text ->
                        dispatch(MemoryWorkspaceIntent.SetFilter(MemoryTab.Saved, text))
                    },
                    onToggleSelected = { address ->
                        dispatch(MemoryWorkspaceIntent.ToggleSelected(MemoryTab.Saved, address))
                    },
                    onOpenMenu = { address ->
                        menuTarget = MenuTarget(MemoryTab.Saved, address)
                    },
                    onClearSelection = {
                        dispatch(MemoryWorkspaceIntent.ClearSelection(MemoryTab.Saved))
                    },
                    onModifySelection = if (!canModify) null else ({ addresses ->
                        modifyTargets = addresses
                    }),
                    modifier = Modifier.weight(1f),
                )
            }

            MemoryTab.Page -> {
                PageControls(
                    bridgeState = bridgeState,
                    onStepPage = onStepPage,
                )

                MemoryDataView(
                    rows = pageRows,
                    selectedAddresses = workspaceState.page.selection,
                    filterText = workspaceState.page.filterText,
                    onFilterTextChanged = { text ->
                        dispatch(MemoryWorkspaceIntent.SetFilter(MemoryTab.Page, text))
                    },
                    onToggleSelected = { address ->
                        dispatch(MemoryWorkspaceIntent.ToggleSelected(MemoryTab.Page, address))
                    },
                    onOpenMenu = { address ->
                        menuTarget = MenuTarget(MemoryTab.Page, address)
                    },
                    onClearSelection = {
                        dispatch(MemoryWorkspaceIntent.ClearSelection(MemoryTab.Page))
                    },
                    onAddSelectionToSaved = {
                        dispatch(MemoryWorkspaceIntent.AddToSaved(workspaceState.page.selection))
                    },
                    onModifySelection = if (!canModify) null else ({ addresses ->
                        modifyTargets = addresses
                    }),
                    modifier = Modifier.weight(1f),
                )
            }
        }

        val target = menuTarget
        MemoryActionMenu(
            expanded = target != null,
            title = target?.addressHex().orEmpty(),
            onDismiss = { menuTarget = null },
            onModify = target?.let { t ->
                if (!canModify) null else ({
                    modifyTargets = setOf(t.address)
                })
            },
            onGoToPage = target?.let { t ->
                {
                    onSelectAddress(t.address)
                    dispatch(MemoryWorkspaceIntent.SwitchTab(MemoryTab.Page))
                }
            },
            onAddToSaved = target?.let { t ->
                if (t.tab == MemoryTab.Saved) null else ({ dispatch(MemoryWorkspaceIntent.AddToSaved(setOf(t.address))) })
            },
            onRemoveFromSaved = target?.let { t ->
                if (t.tab != MemoryTab.Saved) null else ({ dispatch(MemoryWorkspaceIntent.RemoveFromSaved(t.address)) })
            },
            onCopyAddress = target?.let { t ->
                { clipboard.setText(AnnotatedString(t.addressHex())) }
            },
        )

        modifyTargets?.let { targets ->
            MemoryModifyDialog(
                targets = targets,
                onDismiss = { modifyTargets = null },
                onConfirm = { hexBytes ->
                    if (hexBytes.isNotBlank()) {
                        onWriteHexAtAddresses?.invoke(targets, hexBytes)
                            ?: onWriteHexAtAddress?.let { writeOne ->
                                targets.forEach { address ->
                                    writeOne(address, hexBytes)
                                }
                            }
                    }
                    modifyTargets = null
                },
            )
        }
    }
}

@Composable
private fun SearchControls(
    bridgeState: SessionBridgeState,
    onSearchQueryChanged: (String) -> Unit,
    onRunSearch: () -> Unit,
    onRefineSearch: () -> Unit,
    onCycleValueType: () -> Unit,
    onCycleRefineMode: () -> Unit,
    onCycleRegionPreset: () -> Unit,
) {
    OutlinedTextField(
        value = bridgeState.memorySearch.query,
        onValueChange = onSearchQueryChanged,
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp, vertical = 10.dp),
        label = { Text("Search") },
        singleLine = true,
    )

    val controls = remember(bridgeState.memorySearch.valueType, bridgeState.memorySearch.refineMode, bridgeState.memorySearch.regionPreset) {
        listOf(
            "Run" to onRunSearch,
            "Refine" to onRefineSearch,
            bridgeState.memorySearch.valueType.name to onCycleValueType,
            bridgeState.memorySearch.refineMode.name to onCycleRefineMode,
            bridgeState.memorySearch.regionPreset.name to onCycleRegionPreset,
        )
    }
    LazyRow(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp, vertical = 4.dp),
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        items(controls) { (label, action) ->
            TextButton(onClick = action) { Text(label) }
        }
    }
}

@Composable
private fun PageControls(
    bridgeState: SessionBridgeState,
    onStepPage: (Int) -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp, vertical = 10.dp),
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        TextButton(onClick = { onStepPage(-1) }) { Text("Prev") }
        TextButton(onClick = { onStepPage(1) }) { Text("Next") }
        Text(
            text = bridgeState.memoryPage?.let { "focus=${it.focusAddressHex()}" } ?: "no page",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.weight(1f),
            maxLines = 1,
        )
    }
}

private fun MemorySearchResult.toRow(): MemoryDataRow =
    MemoryDataRow(
        address = address,
        title = valueSummary.ifBlank { "match" },
        subtitle = regionName,
        value = previewHex,
    )

private fun MemoryPreviewRow.toRow(): MemoryDataRow =
    MemoryDataRow(
        address = address,
        title = ascii,
        value = hexBytes,
    )

private fun MenuTarget.addressHex(): String = "0x${address.toString(16)}"

private fun MemoryPage.focusAddressHex(): String =
    "0x${focusAddress.toString(16)}"
