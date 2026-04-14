package com.smlc666.lkmdbg.overlay.ui.memory

import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier

@Composable
fun MemoryActionMenu(
    expanded: Boolean,
    onDismiss: () -> Unit,
    modifier: Modifier = Modifier,
    onCopyAddress: () -> Unit = {},
    onCopyValue: () -> Unit = {},
    onRefresh: () -> Unit = {},
    onRemove: () -> Unit = {},
) {
    DropdownMenu(
        expanded = expanded,
        onDismissRequest = onDismiss,
        modifier = modifier,
    ) {
        DropdownMenuItem(
            text = { Text("Copy address") },
            onClick = {
                onCopyAddress()
                onDismiss()
            },
        )
        DropdownMenuItem(
            text = { Text("Copy value") },
            onClick = {
                onCopyValue()
                onDismiss()
            },
        )
        DropdownMenuItem(
            text = { Text("Refresh") },
            onClick = {
                onRefresh()
                onDismiss()
            },
        )
        DropdownMenuItem(
            text = { Text("Remove") },
            onClick = {
                onRemove()
                onDismiss()
            },
        )
    }
}

