@file:OptIn(ExperimentalFoundationApi::class)

package com.smlc666.lkmdbg.overlay.ui.memory

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp

@Composable
fun MemoryDataView(
    rows: List<MemoryDataRow>,
    selectedAddresses: Set<ULong>,
    filterText: String,
    onFilterTextChanged: (String) -> Unit,
    onToggleSelected: (ULong) -> Unit,
    onOpenMenu: (ULong) -> Unit,
    onClearSelection: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val filteredRows by remember(rows, filterText) {
        derivedStateOf {
            val q = filterText.trim()
            if (q.isEmpty()) {
                rows
            } else {
                val needle = q.lowercase()
                rows.filter { row ->
                    buildList(4) {
                        add(row.title)
                        row.subtitle?.let { add(it) }
                        row.value?.let { add(it) }
                        add(row.addressHex)
                        add(row.address.toString())
                    }.any { it.lowercase().contains(needle) }
                }
            }
        }
    }

    Column(
        modifier = modifier.fillMaxSize(),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 12.dp, vertical = 10.dp),
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            OutlinedTextField(
                value = filterText,
                onValueChange = onFilterTextChanged,
                modifier = Modifier.weight(1f),
                singleLine = true,
                label = { Text("Filter") },
            )
            if (selectedAddresses.isNotEmpty()) {
                TextButton(
                    onClick = onClearSelection,
                ) {
                    Text("Clear")
                }
            }
        }

        HorizontalDivider()

        LazyColumn(
            modifier = Modifier.fillMaxSize(),
        ) {
            items(
                items = filteredRows,
                key = { it.address },
            ) { row ->
                val selected = selectedAddresses.contains(row.address)
                MemoryDataRowItem(
                    row = row,
                    selected = selected,
                    onClick = {
                        if (selectedAddresses.isNotEmpty()) {
                            onToggleSelected(row.address)
                        } else {
                            onOpenMenu(row.address)
                        }
                    },
                    onLongPress = {
                        onOpenMenu(row.address)
                    },
                )
                HorizontalDivider()
            }
        }
    }
}

@Composable
private fun MemoryDataRowItem(
    row: MemoryDataRow,
    selected: Boolean,
    onClick: () -> Unit,
    onLongPress: () -> Unit,
) {
    val bg = if (selected) {
        MaterialTheme.colorScheme.primaryContainer
    } else {
        MaterialTheme.colorScheme.surface
    }
    Surface(
        color = bg,
        modifier = Modifier
            .fillMaxWidth()
            .combinedClickable(
                onClick = onClick,
                onLongClick = onLongPress,
            ),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 12.dp, vertical = 10.dp),
        ) {
            Column(
                modifier = Modifier.weight(1f),
            ) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Text(
                        text = row.title,
                        style = MaterialTheme.typography.titleSmall,
                        modifier = Modifier.weight(1f),
                    )
                    Spacer(Modifier.width(10.dp))
                    Text(
                        text = row.addressHex,
                        style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                row.subtitle?.takeIf { it.isNotBlank() }?.let { subtitle ->
                    Spacer(Modifier.height(2.dp))
                    Text(
                        text = subtitle,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                row.value?.takeIf { it.isNotBlank() }?.let { value ->
                    Spacer(Modifier.height(6.dp))
                    Text(
                        text = value,
                        style = MaterialTheme.typography.bodyMedium,
                    )
                }
            }
        }
    }
}
