package com.smlc666.lkmdbg.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable

private val LkmdbgColors = darkColorScheme(
    primary = SignalCyan,
    onPrimary = Graphite,
    secondary = Mint,
    onSecondary = Graphite,
    background = Graphite,
    onBackground = Mist,
    surface = Panel,
    onSurface = Mist,
    surfaceVariant = DeepTeal,
    onSurfaceVariant = Slate,
)

@Composable
fun LkmdbgTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = LkmdbgColors,
        typography = LkmdbgTypography,
        content = content,
    )
}
