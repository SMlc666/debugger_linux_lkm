package com.smlc666.lkmdbg.overlay.ui.memory

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier

@Composable
fun MemoryModifyDialog(
    targets: Set<ULong>,
    onDismiss: () -> Unit,
    onConfirm: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
    var hexBytes by remember { mutableStateOf("") }

    LaunchedEffect(targets) {
        hexBytes = ""
    }

    val title = if (targets.size == 1) {
        val addr = targets.first()
        "Modify 0x${addr.toString(16)}"
    } else {
        "Modify ${targets.size} addresses"
    }

    AlertDialog(
        modifier = modifier,
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            Column(
                modifier = Modifier.fillMaxWidth(),
            ) {
                OutlinedTextField(
                    value = hexBytes,
                    onValueChange = { hexBytes = it },
                    modifier = Modifier.fillMaxWidth(),
                    label = { Text("Hex bytes") },
                    placeholder = { Text("90 90 90 90") },
                    singleLine = false,
                )
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("Cancel") }
        },
        confirmButton = {
            val trimmed = hexBytes.trim()
            TextButton(
                onClick = { onConfirm(trimmed) },
                enabled = trimmed.isNotEmpty(),
            ) {
                Text("Apply")
            }
        },
    )
}
