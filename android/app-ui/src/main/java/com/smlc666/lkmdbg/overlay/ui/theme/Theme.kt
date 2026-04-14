package com.smlc666.lkmdbg.overlay.ui.theme

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val DarkColorScheme = darkColorScheme(
    primary = Color(0xFF669DF6),
    onPrimary = Color(0xFF00315F),
    primaryContainer = Color(0xFF004988),
    onPrimaryContainer = Color(0xFFD0E4FF),
    secondary = Color(0xFF8BCC1B),
    onSecondary = Color(0xFF143800),
    secondaryContainer = Color(0xFF225000),
    onSecondaryContainer = Color(0xFFA5F037),
    tertiary = Color(0xFFFFAEAE),
    onTertiary = Color(0xFF531E20),
    tertiaryContainer = Color(0xFF723434),
    onTertiaryContainer = Color(0xFFFFDAD9),
    error = Color(0xFFFFB4AB),
    onError = Color(0xFF690005),
    errorContainer = Color(0xFF93000A),
    onErrorContainer = Color(0xFFFFDAD9),
    background = Color(0xFF121416),
    onBackground = Color(0xFFC7C6C9),
    surface = Color(0xFF121416),
    onSurface = Color(0xFFC7C6C9),
    surfaceVariant = Color(0xFF43474E),
    onSurfaceVariant = Color(0xFFC3C6CF),
    outline = Color(0xFF8E9199),
)

private val LightColorScheme = lightColorScheme(
    primary = Color(0xFF0061A4),
    onPrimary = Color(0xFFFFFFFF),
    primaryContainer = Color(0xFFD0E4FF),
    onPrimaryContainer = Color(0xFF001D36),
    secondary = Color(0xFF326B00),
    onSecondary = Color(0xFFFFFFFF),
    secondaryContainer = Color(0xFFA5F037),
    onSecondaryContainer = Color(0xFF0B2000),
    tertiary = Color(0xFF904A49),
    onTertiary = Color(0xFFFFFFFF),
    tertiaryContainer = Color(0xFFFFDAD9),
    onTertiaryContainer = Color(0xFF3B080C),
    error = Color(0xFFBA1A1A),
    onError = Color(0xFFFFFFFF),
    errorContainer = Color(0xFFFFDAD9),
    onErrorContainer = Color(0xFF410002),
    background = Color(0xFFFDFBFF),
    onBackground = Color(0xFF1A1C1E),
    surface = Color(0xFFFDFBFF),
    onSurface = Color(0xFF1A1C1E),
    surfaceVariant = Color(0xFFDFE2EB),
    onSurfaceVariant = Color(0xFF43474E),
    outline = Color(0xFF73777F),
)

@Composable
fun LkmdbgTheme(
    darkTheme: Boolean = true, // Force dark theme for debugger overlay by default
    content: @Composable () -> Unit
) {
    val colorScheme = if (darkTheme) {
        DarkColorScheme
    } else {
        LightColorScheme
    }

    MaterialTheme(
        colorScheme = colorScheme,
        content = content
    )
}
