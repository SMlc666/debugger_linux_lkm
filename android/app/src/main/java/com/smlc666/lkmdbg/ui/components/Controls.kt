package com.smlc666.lkmdbg.ui.components

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.smlc666.lkmdbg.ui.theme.DeepTeal
import com.smlc666.lkmdbg.ui.theme.Graphite
import com.smlc666.lkmdbg.ui.theme.Mint
import com.smlc666.lkmdbg.ui.theme.SignalCyan
import com.smlc666.lkmdbg.ui.theme.Slate

internal enum class LkmdbgTagTone {
    Accent,
    Positive,
    Neutral,
}

@Composable
internal fun LkmdbgActionButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    prominent: Boolean = false,
) {
    val containerColor = if (prominent) SignalCyan else DeepTeal.copy(alpha = 0.82f)
    val contentColor = if (prominent) Graphite else MaterialTheme.colorScheme.onSurface

    Button(
        onClick = onClick,
        enabled = enabled,
        modifier = modifier,
        shape = RoundedCornerShape(16.dp),
        colors = ButtonDefaults.buttonColors(
            containerColor = containerColor,
            contentColor = contentColor,
            disabledContainerColor = DeepTeal.copy(alpha = 0.35f),
            disabledContentColor = Slate.copy(alpha = 0.72f),
        ),
    ) {
        Text(text)
    }
}

@Composable
internal fun LkmdbgFilterPill(
    text: String,
    selected: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val containerColor by animateColorAsState(
        targetValue = if (selected) SignalCyan.copy(alpha = 0.92f) else DeepTeal.copy(alpha = 0.34f),
        animationSpec = tween(durationMillis = 140),
        label = "filter_pill_container",
    )
    val contentColor by animateColorAsState(
        targetValue = if (selected) Graphite else MaterialTheme.colorScheme.onSurfaceVariant,
        animationSpec = tween(durationMillis = 140),
        label = "filter_pill_content",
    )
    val borderColor by animateColorAsState(
        targetValue = if (selected) SignalCyan.copy(alpha = 0.24f) else Slate.copy(alpha = 0.18f),
        animationSpec = tween(durationMillis = 140),
        label = "filter_pill_border",
    )

    Surface(
        modifier = modifier.clickable(onClick = onClick),
        color = containerColor,
        contentColor = contentColor,
        shape = RoundedCornerShape(16.dp),
        border = BorderStroke(
            width = 1.dp,
            color = borderColor,
        ),
    ) {
        Row(
            modifier = Modifier
                .padding(horizontal = 12.dp, vertical = 8.dp),
            horizontalArrangement = Arrangement.Center,
        ) {
            Text(
                text = text,
                style = MaterialTheme.typography.labelLarge,
            )
        }
    }
}

@Composable
internal fun LkmdbgInputField(
    value: String,
    onValueChange: (String) -> Unit,
    label: String,
    modifier: Modifier = Modifier,
    placeholder: String? = null,
    singleLine: Boolean = false,
    minLines: Int = 1,
) {
    OutlinedTextField(
        value = value,
        onValueChange = onValueChange,
        modifier = modifier,
        singleLine = singleLine,
        minLines = minLines,
        label = { Text(label) },
        placeholder = placeholder?.let { { Text(it) } },
        shape = RoundedCornerShape(18.dp),
        colors = OutlinedTextFieldDefaults.colors(
            focusedBorderColor = SignalCyan,
            focusedLabelColor = SignalCyan,
            cursorColor = SignalCyan,
            unfocusedBorderColor = Slate.copy(alpha = 0.42f),
            unfocusedLabelColor = MaterialTheme.colorScheme.onSurfaceVariant,
            focusedContainerColor = Color.Transparent,
            unfocusedContainerColor = Color.Transparent,
        ),
    )
}

@Composable
internal fun LkmdbgTag(
    text: String,
    modifier: Modifier = Modifier,
    tone: LkmdbgTagTone = LkmdbgTagTone.Neutral,
) {
    val containerColor = when (tone) {
        LkmdbgTagTone.Accent -> SignalCyan.copy(alpha = 0.16f)
        LkmdbgTagTone.Positive -> Mint.copy(alpha = 0.16f)
        LkmdbgTagTone.Neutral -> DeepTeal.copy(alpha = 0.42f)
    }
    val contentColor = when (tone) {
        LkmdbgTagTone.Accent -> SignalCyan
        LkmdbgTagTone.Positive -> Mint
        LkmdbgTagTone.Neutral -> MaterialTheme.colorScheme.onSurfaceVariant
    }

    Surface(
        modifier = modifier,
        color = containerColor,
        contentColor = contentColor,
        shape = RoundedCornerShape(999.dp),
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 10.dp, vertical = 6.dp),
            horizontalArrangement = Arrangement.Center,
        ) {
            Text(
                text = text,
                style = MaterialTheme.typography.labelMedium,
            )
        }
    }
}
