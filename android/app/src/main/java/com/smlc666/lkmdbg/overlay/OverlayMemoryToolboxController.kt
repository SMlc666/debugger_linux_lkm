package com.smlc666.lkmdbg.overlay

import android.content.Context
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.core.widget.doAfterTextChanged
import com.google.android.material.button.MaterialButton
import com.google.android.material.textfield.TextInputEditText
import com.google.android.material.textfield.TextInputLayout
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.data.SessionBridgeRepository
import com.smlc666.lkmdbg.data.SessionBridgeState
import com.smlc666.lkmdbg.data.WorkspaceSection
import com.smlc666.lkmdbg.ui.theme.LkmdbgButtonTone
import com.smlc666.lkmdbg.ui.theme.copyAlpha
import com.smlc666.lkmdbg.ui.theme.loadLkmdbgColorRoles
import com.smlc666.lkmdbg.ui.theme.styleBody
import com.smlc666.lkmdbg.ui.theme.styleButton
import com.smlc666.lkmdbg.ui.theme.styleHeadline
import com.smlc666.lkmdbg.ui.theme.styleInputLayout
import com.smlc666.lkmdbg.ui.theme.styleSurface
import com.smlc666.lkmdbg.ui.theme.styleTextInput
import kotlin.math.min
import kotlin.math.roundToInt

internal class OverlayMemoryToolboxController(
    private val context: Context,
    private val repository: SessionBridgeRepository,
    private val launchAction: (suspend () -> Unit) -> Unit,
    private val onShowMemoryResults: () -> Unit,
    private val onShowMemoryPage: () -> Unit,
    private val onDismiss: () -> Unit,
) {
    private val roles = loadLkmdbgColorRoles(context)
    private var container: FrameLayout? = null
    private var summaryView: TextView? = null
    private var queryInput: TextInputEditText? = null
    private var addressInput: TextInputEditText? = null
    private var hexInput: TextInputEditText? = null
    private var asciiInput: TextInputEditText? = null

    fun build(density: Float): FrameLayout {
        val drawerWidth = min(
            (360f * density).roundToInt(),
            (context.resources.displayMetrics.widthPixels * 0.84f).roundToInt(),
        )
        val newContainer = FrameLayout(context).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            )
            visibility = View.GONE
            setBackgroundColor(roles.background.copyAlpha(0.46f))
            setOnClickListener { onDismiss() }
        }
        val card = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = FrameLayout.LayoutParams(
                drawerWidth,
                ViewGroup.LayoutParams.MATCH_PARENT,
                Gravity.END,
            ).apply {
                marginStart = (12f * density).roundToInt()
                marginTop = (8f * density).roundToInt()
                marginEnd = (8f * density).roundToInt()
                marginBottom = (8f * density).roundToInt()
            }
            setPadding(
                (12f * density).roundToInt(),
                (12f * density).roundToInt(),
                (12f * density).roundToInt(),
                (12f * density).roundToInt(),
            )
            isClickable = true
            styleSurface(this, roles, roles.surfaceContainerHigh, roles.outlineVariant, radiusDp = 28f)
        }
        card.setOnClickListener { }
        val titleRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            )
        }
        val titleView = TextView(context).apply {
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            text = context.getString(R.string.memory_tools_title)
            styleHeadline(this, roles, 16f)
        }
        val closeButton = MaterialButton(context).apply {
            text = context.getString(R.string.memory_tools_close)
            styleButton(this, roles, LkmdbgButtonTone.Outlined)
            textSize = 11f
            setOnClickListener { onDismiss() }
        }
        titleRow.addView(titleView)
        titleRow.addView(closeButton)

        val scrollView = ScrollView(context).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                0,
                1f,
            )
        }
        val content = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
        }
        val newSummaryView = TextView(context).apply {
            setPadding(0, (6f * density).roundToInt(), 0, 0)
            styleBody(this, roles, 12f, secondary = true)
        }
        val newQueryInput = makeInputField(
            hint = context.getString(R.string.memory_search_query_placeholder),
            inputType = InputType.TYPE_CLASS_TEXT,
        ) {
            repository.updateMemorySearchQuery(it)
        }
        val newAddressInput = makeInputField(
            hint = context.getString(R.string.memory_address_placeholder),
            inputType = InputType.TYPE_CLASS_TEXT,
        ) {
            repository.updateMemoryAddressInput(it)
        }
        val newHexInput = makeInputField(
            hint = context.getString(R.string.memory_write_hex_placeholder),
            inputType = InputType.TYPE_CLASS_TEXT,
        ) {
            repository.updateMemoryWriteHexInput(it)
        }
        val newAsciiInput = makeInputField(
            hint = context.getString(R.string.memory_write_ascii_placeholder),
            inputType = InputType.TYPE_CLASS_TEXT,
        ) {
            repository.updateMemoryWriteAsciiInput(it)
        }

        content.addView(titleRow)
        content.addView(newSummaryView)
        content.addView(makeSectionLabel(R.string.memory_tools_section_search, density))
        content.addView(newQueryInput.first)
        content.addView(
            makeButtonRow(
                density,
                makeActionButton(R.string.memory_action_search) {
                    repository.runMemorySearch()
                    onShowMemoryResults()
                    onDismiss()
                },
                makeActionButton(R.string.memory_action_refine) {
                    repository.refineMemorySearch()
                    onShowMemoryResults()
                    onDismiss()
                },
            ),
        )
        content.addView(makeSectionLabel(R.string.memory_tools_section_jump, density))
        content.addView(newAddressInput.first)
        content.addView(
            makeButtonRow(
                density,
                makeActionButton(R.string.memory_action_jump) {
                    repository.jumpToMemoryAddress()
                    onShowMemoryPage()
                    onDismiss()
                },
                makeActionButton(R.string.memory_action_preview_pc) {
                    repository.previewSelectedPc()
                    onShowMemoryPage()
                    onDismiss()
                },
            ),
        )
        content.addView(makeSectionLabel(R.string.memory_tools_section_write, density))
        content.addView(newHexInput.first)
        content.addView(
            makeButtonRow(
                density,
                makeActionButton(R.string.memory_action_write_hex) {
                    repository.writeHexAtFocus()
                    onShowMemoryPage()
                    onDismiss()
                },
            ),
        )
        content.addView(newAsciiInput.first)
        content.addView(
            makeButtonRow(
                density,
                makeActionButton(R.string.memory_action_write_ascii) {
                    repository.writeAsciiAtFocus()
                    onShowMemoryPage()
                    onDismiss()
                },
            ),
        )

        scrollView.addView(content)
        card.addView(scrollView)
        newContainer.addView(card)

        container = newContainer
        summaryView = newSummaryView
        queryInput = newQueryInput.second
        addressInput = newAddressInput.second
        hexInput = newHexInput.second
        asciiInput = newAsciiInput.second
        return newContainer
    }

    fun render(state: SessionBridgeState, visible: Boolean) {
        val currentContainer = container ?: return
        currentContainer.visibility =
            if (visible && state.workspaceSection == WorkspaceSection.Memory) View.VISIBLE else View.GONE
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

    fun hide() {
        container?.visibility = View.GONE
    }

    private fun makeSectionLabel(textRes: Int, density: Float): TextView =
        TextView(context).apply {
            setPadding(0, (12f * density).roundToInt(), 0, 0)
            text = context.getString(textRes)
            styleBody(this, roles, 13f)
        }

    private fun makeButtonRow(density: Float, vararg buttons: MaterialButton): LinearLayout =
        LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(0, (6f * density).roundToInt(), 0, 0)
            buttons.forEach { addView(it) }
        }

    private fun makeActionButton(textRes: Int, action: suspend () -> Unit): MaterialButton =
        MaterialButton(context).apply {
            text = context.getString(textRes)
            styleButton(this, roles, LkmdbgButtonTone.Tonal)
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

    private fun makeInputField(
        hint: String,
        inputType: Int,
        onChanged: (String) -> Unit,
    ): Pair<TextInputLayout, TextInputEditText> {
        val editText = TextInputEditText(context).apply {
            this.inputType = inputType
            setSingleLine(true)
            styleTextInput(this, roles)
            doAfterTextChanged { editable ->
                onChanged(editable?.toString().orEmpty())
            }
        }
        val layout = TextInputLayout(context).apply {
            this.hint = hint
            setPadding(0, (context.resources.displayMetrics.density * 6f).roundToInt(), 0, 0)
            styleInputLayout(this, roles)
            addView(
                editText,
                LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                ),
            )
        }
        return layout to editText
    }

    private fun syncInput(view: TextInputEditText?, value: String) {
        if (view == null || view.text?.toString() == value)
            return
        view.setText(value)
        view.setSelection(view.text?.length ?: 0)
    }
}
