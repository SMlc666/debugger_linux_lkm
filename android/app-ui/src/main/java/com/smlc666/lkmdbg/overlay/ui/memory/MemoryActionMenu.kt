package com.smlc666.lkmdbg.overlay.ui.memory

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp

@Composable
fun MemoryActionMenu(
    expanded: Boolean,
    title: String,
    onDismiss: () -> Unit,
    modifier: Modifier = Modifier,
    onGoToPage: (() -> Unit)? = null,
    onAddToSaved: (() -> Unit)? = null,
    onRemoveFromSaved: (() -> Unit)? = null,
    onCopyAddress: (() -> Unit)? = null,
) {
    if (!expanded) return

    AlertDialog(
        modifier = modifier,
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            Column(
                modifier = Modifier.fillMaxWidth(),
                verticalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                onGoToPage?.let { action ->
                    TextButton(onClick = { action(); onDismiss() }) { Text("Go to page") }
                }
                onAddToSaved?.let { action ->
                    TextButton(onClick = { action(); onDismiss() }) { Text("Add to saved") }
                }
                onRemoveFromSaved?.let { action ->
                    TextButton(
                        onClick = { action(); onDismiss() },
                    ) {
                        Text(
                            text = "Remove from saved",
                            color = MaterialTheme.colorScheme.error,
                        )
                    }
                }
                onCopyAddress?.let { action ->
                    TextButton(onClick = { action(); onDismiss() }) { Text("Copy address") }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) { Text("Close") }
        },
    )
}

