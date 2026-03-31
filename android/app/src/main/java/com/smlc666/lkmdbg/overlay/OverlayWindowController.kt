package com.smlc666.lkmdbg.overlay

import android.content.res.Resources
import android.graphics.PixelFormat
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import kotlin.math.roundToInt

internal class OverlayWindowController(
    private val resources: Resources,
    private val windowManager: WindowManager,
) {
    private var collapsedDiameterPx = 0
    private var collapsedX = 0
    private var collapsedY = 0

    fun initialize() {
        val density = resources.displayMetrics.density
        collapsedDiameterPx = (72f * density).roundToInt()
        collapsedX = (18f * density).roundToInt()
        collapsedY = (96f * density).roundToInt()
    }

    fun createLayoutParams(expanded: Boolean): WindowManager.LayoutParams =
        WindowManager.LayoutParams(
            if (expanded) WindowManager.LayoutParams.MATCH_PARENT else collapsedDiameterPx,
            if (expanded) WindowManager.LayoutParams.MATCH_PARENT else collapsedDiameterPx,
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            if (expanded) expandedWindowFlags() else collapsedWindowFlags(),
            PixelFormat.TRANSLUCENT,
        ).apply {
            gravity = Gravity.TOP or Gravity.START
            x = if (expanded) 0 else collapsedX
            y = if (expanded) 0 else collapsedY
        }

    fun moveCollapsed(view: View, params: WindowManager.LayoutParams, deltaX: Float, deltaY: Float) {
        collapsedX += deltaX.roundToInt()
        collapsedY += deltaY.roundToInt()
        params.x = collapsedX
        params.y = collapsedY
        windowManager.updateViewLayout(view, params)
    }

    private fun collapsedWindowFlags(): Int =
        WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN or
            WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE

    private fun expandedWindowFlags(): Int =
        WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
}
