package com.smlc666.lkmdbg.overlay

import android.content.Context
import android.view.View
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.TextView
import com.google.android.material.button.MaterialButton
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.data.SessionBridgeState
import com.smlc666.lkmdbg.data.WorkspaceSection
import com.smlc666.lkmdbg.ui.theme.LkmdbgButtonTone
import com.smlc666.lkmdbg.ui.theme.loadLkmdbgColorRoles
import com.smlc666.lkmdbg.ui.theme.styleBody
import com.smlc666.lkmdbg.ui.theme.styleButton
import com.smlc666.lkmdbg.ui.theme.styleSurface
import kotlin.math.roundToInt

internal class OverlayHeaderController(
    private val context: Context,
    private val launchAction: (suspend () -> Unit) -> Unit,
) {
    private val roles = loadLkmdbgColorRoles(context)
    private var statusView: TextView? = null
    private var memoryToolsButton: MaterialButton? = null

    fun build(
        density: Float,
        onCollapse: () -> Unit,
        onClose: () -> Unit,
        onToggleMemoryTools: () -> Unit = {},
    ): LinearLayout {
        val header = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            )
            setPadding(
                (10f * density).toInt(),
                (10f * density).toInt(),
                (10f * density).toInt(),
                (10f * density).toInt(),
            )
            styleSurface(this, roles, roles.surfaceContainerHigh, roles.outlineVariant, radiusDp = 24f)
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
            styleBody(this, roles, 13f)
        }
        val collapseButton = MaterialButton(context).apply {
            text = context.getString(R.string.overlay_action_collapse)
            styleButton(this, roles, LkmdbgButtonTone.Outlined)
            setOnClickListener { onCollapse() }
        }
        val closeButton = MaterialButton(context).apply {
            text = context.getString(R.string.overlay_action_close)
            styleButton(this, roles, LkmdbgButtonTone.Tonal)
            setOnClickListener { onClose() }
        }
        statusRow.addView(newStatusView)
        statusRow.addView(collapseButton)
        statusRow.addView(closeButton)

        header.addView(statusRow)
        statusView = newStatusView
        return header
    }

    fun render(
        state: SessionBridgeState,
        text: String,
        memoryToolsOpen: Boolean,
    ) {
        statusView?.text = text
        memoryToolsButton?.let { button ->
            if (state.workspaceSection == WorkspaceSection.Memory) {
                button.visibility = View.VISIBLE
                styleButton(
                    button,
                    roles,
                    if (memoryToolsOpen) LkmdbgButtonTone.Tonal else LkmdbgButtonTone.Outlined,
                )
            } else {
                button.visibility = View.GONE
            }
        }
    }

    fun clear() {
        statusView = null
        memoryToolsButton = null
    }

    private fun makeActionButton(textRes: Int, action: suspend () -> Unit): MaterialButton {
        return MaterialButton(context).apply {
            text = context.getString(textRes)
            styleButton(this, roles, LkmdbgButtonTone.Filled)
            textSize = 11f
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
}
