package com.smlc666.lkmdbg.overlay

import android.content.Context
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import com.smlc666.lkmdbg.R
import kotlin.math.roundToInt

internal class OverlayHeaderController(
    private val context: Context,
    private val launchAction: (suspend () -> Unit) -> Unit,
) {
    private var statusView: TextView? = null

    fun build(
        density: Float,
        onCollapse: () -> Unit,
        onClose: () -> Unit,
        onConnect: suspend () -> Unit,
        onOpenSession: suspend () -> Unit,
        onRefreshStatus: suspend () -> Unit,
        onRefreshProcesses: suspend () -> Unit,
        onToggleProcessPicker: () -> Unit,
        onRefreshEvents: suspend () -> Unit,
    ): LinearLayout {
        val header = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP,
            )
            setPadding(
                (14f * density).toInt(),
                (14f * density).toInt(),
                (14f * density).toInt(),
                (14f * density).toInt(),
            )
        }
        val statusRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            )
        }
        val newStatusView = TextView(context).apply {
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            textSize = 13f
        }
        val collapseButton = Button(context).apply {
            text = context.getString(R.string.overlay_action_collapse)
            setOnClickListener { onCollapse() }
        }
        val closeButton = Button(context).apply {
            text = context.getString(R.string.overlay_action_close)
            setOnClickListener { onClose() }
        }
        statusRow.addView(newStatusView)
        statusRow.addView(collapseButton)
        statusRow.addView(closeButton)

        val actionRowTop = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            )
            setPadding(0, (8f * density).toInt(), 0, 0)
        }
        val actionRowBottom = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            )
            setPadding(0, (6f * density).toInt(), 0, 0)
        }
        actionRowTop.addView(makeActionButton(R.string.session_action_connect, onConnect))
        actionRowTop.addView(makeActionButton(R.string.session_action_open_session, onOpenSession))
        actionRowTop.addView(makeActionButton(R.string.session_action_refresh, onRefreshStatus))
        actionRowBottom.addView(makeActionButton(R.string.process_action_refresh, onRefreshProcesses))
        actionRowBottom.addView(
            makeActionButton(R.string.process_action_attach) {
                onToggleProcessPicker()
            },
        )
        actionRowBottom.addView(makeActionButton(R.string.event_action_refresh, onRefreshEvents))

        header.addView(statusRow)
        header.addView(actionRowTop)
        header.addView(actionRowBottom)
        statusView = newStatusView
        return header
    }

    fun renderStatus(text: String) {
        statusView?.text = text
    }

    fun clear() {
        statusView = null
    }

    private fun makeActionButton(textRes: Int, action: suspend () -> Unit): Button =
        Button(context).apply {
            text = context.getString(textRes)
            textSize = 12f
            layoutParams = LinearLayout.LayoutParams(
                0,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                1f,
            ).apply {
                marginEnd = (6f * context.resources.displayMetrics.density).roundToInt()
            }
            setOnClickListener {
                launchAction {
                    action()
                }
            }
        }
}
