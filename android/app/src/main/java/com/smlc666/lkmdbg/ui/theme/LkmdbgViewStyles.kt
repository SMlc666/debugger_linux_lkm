package com.smlc666.lkmdbg.ui.theme

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.drawable.GradientDrawable
import android.util.TypedValue
import android.view.View
import android.widget.TextView
import androidx.annotation.ColorInt
import com.google.android.material.button.MaterialButton
import com.google.android.material.textfield.TextInputEditText
import com.google.android.material.textfield.TextInputLayout

internal enum class LkmdbgButtonTone {
    Filled,
    Tonal,
    Outlined,
}

internal fun styleSurface(
    view: View,
    roles: LkmdbgColorRoles,
    @ColorInt fill: Int,
    @ColorInt stroke: Int = roles.outlineVariant,
    radiusDp: Float = 24f,
    strokeDp: Float = 1f,
) {
    view.background = GradientDrawable().apply {
        shape = GradientDrawable.RECTANGLE
        cornerRadius = view.context.dp(radiusDp)
        setColor(fill)
        setStroke(view.context.dp(strokeDp).toInt().coerceAtLeast(1), stroke)
    }
}

internal fun styleHeadline(textView: TextView, roles: LkmdbgColorRoles, sizeSp: Float) {
    textView.setTextColor(roles.onSurface)
    textView.setTextSize(TypedValue.COMPLEX_UNIT_SP, sizeSp)
}

internal fun styleBody(textView: TextView, roles: LkmdbgColorRoles, sizeSp: Float, secondary: Boolean = false) {
    textView.setTextColor(if (secondary) roles.onSurfaceVariant else roles.onSurface)
    textView.setTextSize(TypedValue.COMPLEX_UNIT_SP, sizeSp)
}

internal fun styleButton(
    button: MaterialButton,
    roles: LkmdbgColorRoles,
    tone: LkmdbgButtonTone,
) {
    val (background, text, stroke) = when (tone) {
        LkmdbgButtonTone.Filled -> Triple(roles.primary, roles.onPrimary, roles.primary)
        LkmdbgButtonTone.Tonal -> Triple(roles.primaryContainer, roles.onPrimaryContainer, roles.outlineVariant)
        LkmdbgButtonTone.Outlined -> Triple(roles.surfaceContainerHigh, roles.onSurface, roles.outline)
    }
    button.backgroundTintList = ColorStateList.valueOf(background)
    button.setTextColor(text)
    button.strokeColor = ColorStateList.valueOf(stroke)
    button.strokeWidth = button.context.dp(1f).toInt().coerceAtLeast(1)
    button.cornerRadius = button.context.dp(18f).toInt()
    button.rippleColor = ColorStateList.valueOf(roles.primary.copyAlpha(0.18f))
    button.insetTop = 0
    button.insetBottom = 0
    button.insetLeft = 0
    button.insetRight = 0
    button.elevation = 0f
    button.setPadding(
        button.context.dp(10f).toInt(),
        button.context.dp(10f).toInt(),
        button.context.dp(10f).toInt(),
        button.context.dp(10f).toInt(),
    )
}

internal fun styleInputLayout(layout: TextInputLayout, roles: LkmdbgColorRoles) {
    layout.boxBackgroundMode = TextInputLayout.BOX_BACKGROUND_OUTLINE
    layout.setBoxBackgroundColor(roles.surfaceContainerHigh)
    layout.setHintTextColor(ColorStateList.valueOf(roles.onSurfaceVariant))
    layout.setBoxStrokeColorStateList(
        ColorStateList(
            arrayOf(
                intArrayOf(android.R.attr.state_focused),
                intArrayOf(),
            ),
            intArrayOf(
                roles.primary,
                roles.outlineVariant,
            ),
        ),
    )
    val radius = layout.context.dp(20f)
    layout.setBoxCornerRadii(radius, radius, radius, radius)
}

internal fun styleTextInput(editText: TextInputEditText, roles: LkmdbgColorRoles) {
    editText.setTextColor(roles.onSurface)
    editText.setHintTextColor(roles.onSurfaceVariant)
    editText.highlightColor = roles.primary.copyAlpha(0.24f)
}

private fun Context.dp(value: Float): Float =
    TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value, resources.displayMetrics)

@ColorInt
internal fun Int.copyAlpha(alpha: Float): Int {
    val a = (alpha.coerceIn(0f, 1f) * 255f).toInt()
    return (this and 0x00ffffff) or (a shl 24)
}
