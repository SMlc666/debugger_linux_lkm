package com.smlc666.lkmdbg.ui.theme

import android.content.Context
import androidx.annotation.ColorInt
import androidx.core.content.ContextCompat
import com.smlc666.lkmdbg.R

internal data class LkmdbgColorRoles(
    @ColorInt val background: Int,
    @ColorInt val surface: Int,
    @ColorInt val surfaceContainer: Int,
    @ColorInt val surfaceContainerHigh: Int,
    @ColorInt val surfaceVariant: Int,
    @ColorInt val primary: Int,
    @ColorInt val onPrimary: Int,
    @ColorInt val primaryContainer: Int,
    @ColorInt val onPrimaryContainer: Int,
    @ColorInt val secondary: Int,
    @ColorInt val onSecondary: Int,
    @ColorInt val secondaryContainer: Int,
    @ColorInt val onSecondaryContainer: Int,
    @ColorInt val tertiary: Int,
    @ColorInt val onTertiary: Int,
    @ColorInt val tertiaryContainer: Int,
    @ColorInt val onTertiaryContainer: Int,
    @ColorInt val error: Int,
    @ColorInt val onError: Int,
    @ColorInt val errorContainer: Int,
    @ColorInt val onErrorContainer: Int,
    @ColorInt val outline: Int,
    @ColorInt val outlineVariant: Int,
    @ColorInt val onSurface: Int,
    @ColorInt val onSurfaceVariant: Int,
)

internal fun loadLkmdbgColorRoles(context: Context): LkmdbgColorRoles =
    LkmdbgColorRoles(
        background = context.color(R.color.lkmdbg_dark_background),
        surface = context.color(R.color.lkmdbg_dark_surface),
        surfaceContainer = context.color(R.color.lkmdbg_dark_surface_container),
        surfaceContainerHigh = context.color(R.color.lkmdbg_dark_surface_container_high),
        surfaceVariant = context.color(R.color.lkmdbg_dark_surface_variant),
        primary = context.color(R.color.lkmdbg_dark_primary),
        onPrimary = context.color(R.color.lkmdbg_dark_on_primary),
        primaryContainer = context.color(R.color.lkmdbg_dark_primary_container),
        onPrimaryContainer = context.color(R.color.lkmdbg_dark_on_primary_container),
        secondary = context.color(R.color.lkmdbg_dark_secondary),
        onSecondary = context.color(R.color.lkmdbg_dark_on_secondary),
        secondaryContainer = context.color(R.color.lkmdbg_dark_secondary_container),
        onSecondaryContainer = context.color(R.color.lkmdbg_dark_on_secondary_container),
        tertiary = context.color(R.color.lkmdbg_dark_tertiary),
        onTertiary = context.color(R.color.lkmdbg_dark_on_tertiary),
        tertiaryContainer = context.color(R.color.lkmdbg_dark_tertiary_container),
        onTertiaryContainer = context.color(R.color.lkmdbg_dark_on_tertiary_container),
        error = context.color(R.color.lkmdbg_dark_error),
        onError = context.color(R.color.lkmdbg_dark_on_error),
        errorContainer = context.color(R.color.lkmdbg_dark_error_container),
        onErrorContainer = context.color(R.color.lkmdbg_dark_on_error_container),
        outline = context.color(R.color.lkmdbg_dark_outline),
        outlineVariant = context.color(R.color.lkmdbg_dark_outline_variant),
        onSurface = context.color(R.color.lkmdbg_dark_on_surface),
        onSurfaceVariant = context.color(R.color.lkmdbg_dark_on_surface_variant),
    )

@ColorInt
private fun Context.color(resId: Int): Int = ContextCompat.getColor(this, resId)
