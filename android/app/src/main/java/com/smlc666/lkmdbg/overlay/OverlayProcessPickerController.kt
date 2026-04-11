package com.smlc666.lkmdbg.overlay

import android.content.Context
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import com.google.android.material.button.MaterialButton
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.data.ProcessFilter
import com.smlc666.lkmdbg.data.ResolvedProcessRecord
import com.smlc666.lkmdbg.data.SessionBridgeRepository
import com.smlc666.lkmdbg.data.SessionBridgeState
import com.smlc666.lkmdbg.data.WorkspaceSection
import com.smlc666.lkmdbg.shell.AppIconLoader
import com.smlc666.lkmdbg.ui.theme.LkmdbgButtonTone
import com.smlc666.lkmdbg.ui.theme.copyAlpha
import com.smlc666.lkmdbg.ui.theme.loadLkmdbgColorRoles
import com.smlc666.lkmdbg.ui.theme.styleBody
import com.smlc666.lkmdbg.ui.theme.styleButton
import com.smlc666.lkmdbg.ui.theme.styleHeadline
import com.smlc666.lkmdbg.ui.theme.styleSurface
import kotlin.math.roundToInt

internal class OverlayProcessPickerController(
    private val context: Context,
    private val repository: SessionBridgeRepository,
    private val iconLoader: AppIconLoader,
    private val launchAction: (suspend () -> Unit) -> Unit,
) {
    private val roles = loadLkmdbgColorRoles(context)
    private var container: FrameLayout? = null
    private var summaryView: TextView? = null
    private var listView: LinearLayout? = null
    private val filterButtons = linkedMapOf<ProcessFilter, MaterialButton>()

    fun build(density: Float): FrameLayout {
        val newContainer = FrameLayout(context).apply {
            visibility = View.GONE
            setBackgroundColor(roles.background.copyAlpha(0.72f))
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            )
            setOnClickListener { hide() }
        }
        val card = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.CENTER_HORIZONTAL,
            ).apply {
                topMargin = (12f * density).roundToInt()
                marginStart = (14f * density).roundToInt()
                marginEnd = (14f * density).roundToInt()
            }
            setPadding((14f * density).roundToInt(), (14f * density).roundToInt(), (14f * density).roundToInt(), (14f * density).roundToInt())
            isClickable = true
            styleSurface(this, roles, roles.surfaceContainerHigh, roles.outlineVariant, radiusDp = 28f)
        }
        card.setOnClickListener { }

        val title = TextView(context).apply {
            text = context.getString(R.string.process_panel_title)
            styleHeadline(this, roles, 16f)
        }
        val filterRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(0, (10f * density).roundToInt(), 0, 0)
        }
        filterRow.addView(makeFilterButton(R.string.process_filter_all, ProcessFilter.All))
        filterRow.addView(makeFilterButton(R.string.process_filter_android, ProcessFilter.AndroidApps))
        filterRow.addView(makeFilterButton(R.string.process_filter_cmdline, ProcessFilter.CommandLine))
        filterRow.addView(makeFilterButton(R.string.process_filter_user, ProcessFilter.UserApps))

        val newSummaryView = TextView(context).apply {
            setPadding(0, (10f * density).roundToInt(), 0, (8f * density).roundToInt())
            styleBody(this, roles, 12f, secondary = true)
        }
        val scrollView = ScrollView(context).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                (320f * density).roundToInt(),
            )
        }
        val newListView = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
        }
        scrollView.addView(newListView)

        card.addView(title)
        card.addView(filterRow)
        card.addView(newSummaryView)
        card.addView(scrollView)
        newContainer.addView(card)
        container = newContainer
        summaryView = newSummaryView
        listView = newListView
        return newContainer
    }

    fun toggle(state: SessionBridgeState) {
        val currentContainer = container ?: return
        currentContainer.visibility =
            if (currentContainer.visibility == View.VISIBLE) View.GONE else View.VISIBLE
        if (currentContainer.visibility == View.VISIBLE)
            render(state)
    }

    fun hide() {
        container?.visibility = View.GONE
    }

    fun isVisible(): Boolean {
        return container?.visibility == View.VISIBLE
    }

    fun render(state: SessionBridgeState) {
        val currentSummary = summaryView ?: return
        val currentList = listView ?: return
        val filtered = state.processes.filter { state.processFilter.matches(it) }
        filterButtons.forEach { (filter, button) ->
            styleButton(
                button,
                roles,
                if (filter == state.processFilter) LkmdbgButtonTone.Filled else LkmdbgButtonTone.Outlined,
            )
        }
        currentSummary.text = context.getString(
            R.string.process_summary_counts,
            state.processes.size,
            state.processes.count { it.isAndroidApp },
            state.processes.count { !it.isAndroidApp },
            state.processes.count { it.isAndroidApp && it.isSystemApp },
            state.processes.count { it.isAndroidApp && !it.isSystemApp },
        )
        currentList.removeAllViews()
        if (filtered.isEmpty()) {
            currentList.addView(
                TextView(context).apply {
                    text = context.getString(R.string.process_filter_empty)
                    styleBody(this, roles, 13f)
                },
            )
            return
        }
        filtered.forEach { process ->
            currentList.addView(makeProcessRow(process))
        }
    }

    fun clear() {
        container = null
        summaryView = null
        listView = null
        filterButtons.clear()
    }

    private fun makeFilterButton(textRes: Int, filter: ProcessFilter): MaterialButton {
        return MaterialButton(context).apply {
            text = context.getString(textRes)
            textSize = 11f
            styleButton(this, roles, LkmdbgButtonTone.Outlined)
            layoutParams = LinearLayout.LayoutParams(
                0,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                1f,
            ).apply {
                marginEnd = (6f * context.resources.displayMetrics.density).roundToInt()
            }
            setOnClickListener {
                repository.updateProcessFilter(filter)
                render(repository.state.value)
            }
            filterButtons[filter] = this
        }
    }

    private fun makeProcessRow(process: ResolvedProcessRecord): View {
        val density = context.resources.displayMetrics.density
        val roles = loadLkmdbgColorRoles(context)
        return LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            ).apply {
                bottomMargin = (6f * density).roundToInt()
            }
            setPadding(
                (10f * density).roundToInt(),
                (8f * density).roundToInt(),
                (10f * density).roundToInt(),
                (8f * density).roundToInt(),
            )
            styleSurface(this, roles, roles.surfaceContainer, roles.outlineVariant, radiusDp = 22f)
            val iconView = ImageView(context).apply {
                layoutParams = LinearLayout.LayoutParams(
                    (28f * density).roundToInt(),
                    (28f * density).roundToInt(),
                ).apply {
                    marginEnd = (10f * density).roundToInt()
                    gravity = Gravity.CENTER_VERTICAL
                }
                iconLoader.load(process.iconPackageName)?.let(::setImageDrawable)
            }
            val textColumn = LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                layoutParams = LinearLayout.LayoutParams(
                    0,
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                    1f,
                )
            }
            val titleView = TextView(context).apply {
                text = process.displayName
                styleBody(this, roles, 13f)
            }
            val subtitleView = TextView(context).apply {
                text = buildString {
                    append("pid=")
                    append(process.pid)
                    append(" uid=")
                    append(process.uid)
                    if (!process.packageName.isNullOrBlank()) {
                        append("  ")
                        append(process.packageName)
                    }
                }
                styleBody(this, roles, 11f, secondary = true)
            }
            val kindView = TextView(context).apply {
                text = when {
                    process.isAndroidApp && process.isSystemApp -> context.getString(R.string.process_kind_system_app)
                    process.isAndroidApp -> context.getString(R.string.process_kind_user_app)
                    else -> context.getString(R.string.process_kind_command_line)
                }
                setTextColor(roles.secondary)
                textSize = 11f
            }
            textColumn.addView(titleView)
            textColumn.addView(subtitleView)
            addView(iconView)
            addView(textColumn)
            addView(kindView)
            setOnClickListener {
                launchAction {
                    if (repository.attachProcess(process.pid)) {
                        repository.updateWorkspaceSection(WorkspaceSection.Memory)
                    }
                    hide()
                }
            }
        }
    }
}
