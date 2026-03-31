package com.smlc666.lkmdbg.overlay

import android.content.Context
import android.graphics.Color
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.data.ProcessFilter
import com.smlc666.lkmdbg.data.ResolvedProcessRecord
import com.smlc666.lkmdbg.data.SessionBridgeRepository
import com.smlc666.lkmdbg.data.SessionBridgeState
import com.smlc666.lkmdbg.shell.AppIconLoader
import kotlin.math.roundToInt

internal class OverlayProcessPickerController(
    private val context: Context,
    private val repository: SessionBridgeRepository,
    private val iconLoader: AppIconLoader,
    private val launchAction: (suspend () -> Unit) -> Unit,
) {
    private var container: FrameLayout? = null
    private var summaryView: TextView? = null
    private var listView: LinearLayout? = null

    fun build(density: Float): FrameLayout {
        val newContainer = FrameLayout(context).apply {
            visibility = View.GONE
            setBackgroundColor(0x88050B12.toInt())
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            )
            setOnClickListener { hide() }
        }
        val card = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.argb(238, 12, 21, 29))
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.CENTER_HORIZONTAL,
            ).apply {
                topMargin = (92f * density).roundToInt()
                marginStart = (14f * density).roundToInt()
                marginEnd = (14f * density).roundToInt()
            }
            setPadding((14f * density).roundToInt(), (14f * density).roundToInt(), (14f * density).roundToInt(), (14f * density).roundToInt())
            isClickable = true
        }
        card.setOnClickListener { }

        val title = TextView(context).apply {
            text = context.getString(R.string.process_panel_title)
            textSize = 16f
            setTextColor(Color.WHITE)
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
            textSize = 12f
            setTextColor(Color.argb(230, 205, 226, 240))
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

    fun render(state: SessionBridgeState) {
        val currentSummary = summaryView ?: return
        val currentList = listView ?: return
        val filtered = state.processes.filter { state.processFilter.matches(it) }
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
                    setTextColor(Color.WHITE)
                    textSize = 13f
                },
            )
            return
        }
        filtered.take(24).forEach { process ->
            currentList.addView(makeProcessRow(process))
        }
    }

    fun clear() {
        container = null
        summaryView = null
        listView = null
    }

    private fun makeFilterButton(textRes: Int, filter: ProcessFilter): Button =
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
                repository.updateProcessFilter(filter)
                render(repository.state.value)
            }
        }

    private fun makeProcessRow(process: ResolvedProcessRecord): View {
        val density = context.resources.displayMetrics.density
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
            setBackgroundColor(Color.argb(210, 18, 32, 44))
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
                textSize = 13f
                setTextColor(Color.WHITE)
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
                textSize = 11f
                setTextColor(Color.argb(230, 197, 214, 225))
            }
            val kindView = TextView(context).apply {
                text = when {
                    process.isAndroidApp && process.isSystemApp -> context.getString(R.string.process_kind_system_app)
                    process.isAndroidApp -> context.getString(R.string.process_kind_user_app)
                    else -> context.getString(R.string.process_kind_command_line)
                }
                textSize = 11f
                setTextColor(Color.argb(255, 101, 222, 215))
            }
            textColumn.addView(titleView)
            textColumn.addView(subtitleView)
            addView(iconView)
            addView(textColumn)
            addView(kindView)
            setOnClickListener {
                launchAction {
                    repository.attachProcess(process.pid)
                    hide()
                }
            }
        }
    }
}
