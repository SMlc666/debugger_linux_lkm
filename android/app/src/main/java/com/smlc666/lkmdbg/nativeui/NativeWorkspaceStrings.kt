package com.smlc666.lkmdbg.nativeui

import android.content.Context
import com.smlc666.lkmdbg.R

internal data class NativeWorkspaceStrings(
    val title: String,
    val session: String,
    val processes: String,
    val memory: String,
    val threads: String,
    val events: String,
    val sessionSubtitle: String,
    val processesSubtitle: String,
    val memorySubtitle: String,
    val threadsSubtitle: String,
    val eventsSubtitle: String,
    val connected: String,
    val sessionOpen: String,
    val hook: String,
    val processCount: String,
    val threadCount: String,
    val eventCount: String,
    val boolYes: String,
    val boolNo: String,
)

private fun cleanMetricLabel(raw: String): String =
    raw.substringBefore('%')
        .trim()
        .trimEnd(':', '：', ' ')

internal fun loadNativeWorkspaceStrings(context: Context): NativeWorkspaceStrings =
    NativeWorkspaceStrings(
        title = context.getString(R.string.overlay_title),
        session = context.getString(R.string.workspace_session),
        processes = context.getString(R.string.workspace_processes),
        memory = context.getString(R.string.workspace_memory),
        threads = context.getString(R.string.workspace_threads),
        events = context.getString(R.string.workspace_events),
        sessionSubtitle = context.getString(R.string.workspace_session_subtitle),
        processesSubtitle = context.getString(R.string.workspace_processes_subtitle),
        memorySubtitle = context.getString(R.string.workspace_memory_subtitle),
        threadsSubtitle = context.getString(R.string.workspace_threads_subtitle),
        eventsSubtitle = context.getString(R.string.workspace_events_subtitle),
        connected = cleanMetricLabel(context.getString(R.string.session_flag_connected)),
        sessionOpen = cleanMetricLabel(context.getString(R.string.session_flag_open)),
        hook = cleanMetricLabel(context.getString(R.string.session_flag_hook_active)),
        processCount = context.getString(R.string.process_summary_total, 0).substringBefore(' '),
        threadCount = context.getString(R.string.thread_panel_title).substringBefore(' '),
        eventCount = context.getString(R.string.event_panel_title).substringBefore(' '),
        boolYes = context.getString(R.string.bool_yes),
        boolNo = context.getString(R.string.bool_no),
    )
