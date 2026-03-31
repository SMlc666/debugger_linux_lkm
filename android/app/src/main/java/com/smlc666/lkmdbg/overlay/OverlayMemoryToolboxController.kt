package com.smlc666.lkmdbg.overlay

import android.content.Context
import android.graphics.Color
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import androidx.core.widget.doAfterTextChanged
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.data.SessionBridgeRepository
import com.smlc666.lkmdbg.data.SessionBridgeState
import com.smlc666.lkmdbg.data.WorkspaceSection
import kotlin.math.roundToInt

internal class OverlayMemoryToolboxController(
    private val context: Context,
    private val repository: SessionBridgeRepository,
    private val launchAction: (suspend () -> Unit) -> Unit,
) {
    private var container: LinearLayout? = null
    private var summaryView: TextView? = null
    private var queryInput: EditText? = null
    private var addressInput: EditText? = null
    private var hexInput: EditText? = null
    private var asciiInput: EditText? = null

    fun build(density: Float): LinearLayout {
        val newContainer = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.argb(232, 9, 18, 26))
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM,
            ).apply {
                marginStart = (12f * density).roundToInt()
                marginEnd = (12f * density).roundToInt()
                bottomMargin = (12f * density).roundToInt()
            }
            setPadding(
                (12f * density).roundToInt(),
                (12f * density).roundToInt(),
                (12f * density).roundToInt(),
                (12f * density).roundToInt(),
            )
            visibility = View.GONE
        }
        val newSummaryView = TextView(context).apply {
            textSize = 12f
            setTextColor(Color.argb(230, 205, 226, 240))
        }
        val newQueryInput = makeEditText(
            hint = context.getString(R.string.memory_search_query_placeholder),
            inputType = InputType.TYPE_CLASS_TEXT,
        ) {
            repository.updateMemorySearchQuery(it)
        }
        val newAddressInput = makeEditText(
            hint = context.getString(R.string.memory_address_placeholder),
            inputType = InputType.TYPE_CLASS_TEXT,
        ) {
            repository.updateMemoryAddressInput(it)
        }
        val newHexInput = makeEditText(
            hint = context.getString(R.string.memory_write_hex_placeholder),
            inputType = InputType.TYPE_CLASS_TEXT,
        ) {
            repository.updateMemoryWriteHexInput(it)
        }
        val newAsciiInput = makeEditText(
            hint = context.getString(R.string.memory_write_ascii_placeholder),
            inputType = InputType.TYPE_CLASS_TEXT,
        ) {
            repository.updateMemoryWriteAsciiInput(it)
        }

        newContainer.addView(newSummaryView)
        newContainer.addView(makeLabeledRow(density, R.string.memory_search_query_label, newQueryInput))
        newContainer.addView(
            makeButtonRow(
                density,
                makeActionButton(R.string.memory_action_search) { repository.runMemorySearch() },
                makeActionButton(R.string.memory_action_refine) { repository.refineMemorySearch() },
            ),
        )
        newContainer.addView(makeLabeledRow(density, R.string.memory_address_label, newAddressInput))
        newContainer.addView(
            makeButtonRow(
                density,
                makeActionButton(R.string.memory_action_jump) { repository.jumpToMemoryAddress() },
                makeActionButton(R.string.memory_action_preview_pc) { repository.previewSelectedPc() },
            ),
        )
        newContainer.addView(makeLabeledRow(density, R.string.memory_write_hex_label, newHexInput))
        newContainer.addView(
            makeButtonRow(
                density,
                makeActionButton(R.string.memory_action_write_hex) { repository.writeHexAtFocus() },
            ),
        )
        newContainer.addView(makeLabeledRow(density, R.string.memory_write_ascii_label, newAsciiInput))
        newContainer.addView(
            makeButtonRow(
                density,
                makeActionButton(R.string.memory_action_write_ascii) { repository.writeAsciiAtFocus() },
            ),
        )

        container = newContainer
        summaryView = newSummaryView
        queryInput = newQueryInput
        addressInput = newAddressInput
        hexInput = newHexInput
        asciiInput = newAsciiInput
        return newContainer
    }

    fun render(state: SessionBridgeState) {
        val currentContainer = container ?: return
        val visible = state.workspaceSection == WorkspaceSection.Memory
        currentContainer.visibility = if (visible) View.VISIBLE else View.GONE
        if (!visible)
            return

        summaryView?.text = buildString {
            append(context.getString(state.memorySearch.valueType.labelRes))
            append(" · ")
            append(context.getString(state.memorySearch.refineMode.labelRes))
            append(" · ")
            append(context.getString(state.memorySearch.regionPreset.labelRes))
            if (state.memorySearch.summary.isNotBlank()) {
                append('\n')
                append(state.memorySearch.summary)
            }
        }
        syncInput(queryInput, state.memorySearch.query)
        syncInput(addressInput, state.memoryAddressInput)
        syncInput(hexInput, state.memoryWriteHexInput)
        syncInput(asciiInput, state.memoryWriteAsciiInput)
    }

    fun clear() {
        container = null
        summaryView = null
        queryInput = null
        addressInput = null
        hexInput = null
        asciiInput = null
    }

    private fun makeLabeledRow(density: Float, labelRes: Int, input: EditText): LinearLayout =
        LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, (8f * density).roundToInt(), 0, 0)
            addView(
                TextView(context).apply {
                    text = context.getString(labelRes)
                    textSize = 11f
                    setTextColor(Color.argb(210, 200, 220, 232))
                },
            )
            addView(input)
        }

    private fun makeButtonRow(density: Float, vararg buttons: Button): LinearLayout =
        LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(0, (6f * density).roundToInt(), 0, 0)
            buttons.forEach { addView(it) }
        }

    private fun makeActionButton(textRes: Int, action: suspend () -> Unit): Button =
        Button(context).apply {
            text = context.getString(textRes)
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

    private fun makeEditText(
        hint: String,
        inputType: Int,
        onChanged: (String) -> Unit,
    ): EditText =
        EditText(context).apply {
            this.hint = hint
            this.inputType = inputType
            setSingleLine(true)
            doAfterTextChanged { editable ->
                onChanged(editable?.toString().orEmpty())
            }
        }

    private fun syncInput(view: EditText?, value: String) {
        if (view == null || view.text.toString() == value)
            return
        view.setText(value)
        view.setSelection(view.text.length)
    }
}
