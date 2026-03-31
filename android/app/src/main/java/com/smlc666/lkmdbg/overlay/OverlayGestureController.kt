package com.smlc666.lkmdbg.overlay

import android.view.MotionEvent
import kotlin.math.abs

internal class OverlayGestureController(
    private val clickSlopPx: Float,
) {
    private var lastTouchRawX = 0f
    private var lastTouchRawY = 0f
    private var dragDistancePx = 0f

    fun onCollapsedTouch(
        event: MotionEvent,
        moveBy: (dx: Float, dy: Float) -> Unit,
        expand: () -> Unit,
    ): Boolean =
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                lastTouchRawX = event.rawX
                lastTouchRawY = event.rawY
                dragDistancePx = 0f
                true
            }

            MotionEvent.ACTION_MOVE -> {
                val dx = event.rawX - lastTouchRawX
                val dy = event.rawY - lastTouchRawY
                lastTouchRawX = event.rawX
                lastTouchRawY = event.rawY
                dragDistancePx += abs(dx) + abs(dy)
                moveBy(dx, dy)
                true
            }

            MotionEvent.ACTION_UP -> {
                if (dragDistancePx <= clickSlopPx)
                    expand()
                true
            }

            MotionEvent.ACTION_CANCEL -> {
                dragDistancePx = 0f
                true
            }

            else -> false
        }
}
