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
    val connected: String,
    val sessionOpen: String,
    val hook: String,
    val processCount: String,
    val threadCount: String,
    val eventCount: String,
)

internal fun loadNativeWorkspaceStrings(context: Context): NativeWorkspaceStrings =
    NativeWorkspaceStrings(
        title = context.getString(R.string.overlay_title),
        session = context.getString(R.string.workspace_session),
        processes = context.getString(R.string.workspace_processes),
        memory = context.getString(R.string.workspace_memory),
        threads = context.getString(R.string.workspace_threads),
        events = context.getString(R.string.workspace_events),
        connected = context.getString(R.string.session_flag_connected).substringBefore(':'),
        sessionOpen = context.getString(R.string.session_flag_open).substringBefore(':'),
        hook = context.getString(R.string.session_flag_hook_active).substringBefore(':'),
        processCount = context.getString(R.string.process_summary_total, 0).substringBefore(' '),
        threadCount = context.getString(R.string.thread_panel_title).substringBefore(' '),
        eventCount = context.getString(R.string.event_panel_title).substringBefore(' '),
    )
