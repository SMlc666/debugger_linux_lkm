package com.smlc666.lkmdbg.nativeui

import android.content.Context
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.data.ProcessFilter
import com.smlc666.lkmdbg.data.ResolvedProcessRecord
import com.smlc666.lkmdbg.data.SessionBridgeState

internal data class NativeWorkspaceSnapshot(
    val expanded: Boolean,
    val busy: Boolean,
    val connected: Boolean,
    val sessionOpen: Boolean,
    val hookActive: Int,
    val targetPid: Int,
    val targetTid: Int,
    val eventQueueDepth: Int,
    val processCount: Int,
    val threadCount: Int,
    val eventCount: Int,
    val imageCount: Int,
    val vmaCount: Int,
    val sessionPrimary: String,
    val sessionSecondary: String,
    val processPrimary: String,
    val processSecondary: String,
    val memoryPrimary: String,
    val memorySecondary: String,
    val threadPrimary: String,
    val threadSecondary: String,
    val eventPrimary: String,
    val eventSecondary: String,
    val footerMessage: String,
)

private fun hex64(value: ULong): String = "0x${value.toString(16)}"

private fun processFilterLabel(context: Context, filter: ProcessFilter): String =
    when (filter) {
        ProcessFilter.All -> context.getString(R.string.process_filter_all)
        ProcessFilter.AndroidApps -> context.getString(R.string.process_filter_android)
        ProcessFilter.CommandLine -> context.getString(R.string.process_filter_cmdline)
        ProcessFilter.SystemApps -> context.getString(R.string.process_filter_system)
        ProcessFilter.UserApps -> context.getString(R.string.process_filter_user)
    }

private fun processHeadline(process: ResolvedProcessRecord): String =
    buildString {
        append(process.displayName)
        append(" · pid=")
        append(process.pid)
        if (!process.packageName.isNullOrBlank()) {
            append(" · ")
            append(process.packageName)
        }
    }

internal fun SessionBridgeState.toNativeWorkspaceSnapshot(
    context: Context,
    expanded: Boolean,
): NativeWorkspaceSnapshot {
    val selectedThread = selectedThreadTid?.let { tid -> threads.firstOrNull { it.tid == tid } }
        ?: threads.firstOrNull()
    val latestEvent = recentEvents.firstOrNull()
    val page = memoryPage
    val firstProcess = processes.firstOrNull()
    val sessionPrimary = context.getString(
        R.string.overlay_status_template,
        snapshot.transport,
        snapshot.targetPid,
        snapshot.targetTid,
        "0x${snapshot.sessionId.toString(16)}",
    )
    val sessionSecondary = if (busy) {
        context.getString(R.string.workspace_state_busy, lastMessage)
    } else {
        lastMessage
    }
    val processPrimary = context.getString(
        R.string.workspace_process_summary,
        processFilterLabel(context, processFilter),
        processes.size,
        processes.count { it.isAndroidApp },
        processes.count { !it.isAndroidApp },
    )
    val processSecondary = firstProcess?.let(::processHeadline)
        ?: context.getString(R.string.process_empty)
    val memoryPrimary = if (page != null) {
        context.getString(
            R.string.workspace_memory_summary,
            hex64(page.focusAddress),
            page.bytes.size,
            vmas.size,
            images.size,
        )
    } else {
        context.getString(
            R.string.workspace_memory_summary_idle,
            vmas.size,
            images.size,
        )
    }
    val memorySecondary = page?.region?.let { region ->
        context.getString(
            R.string.workspace_memory_region_summary,
            region.name,
            hex64(region.startAddr),
            hex64(region.endAddr),
        )
    } ?: context.getString(R.string.memory_page_region_none)
    val threadPrimary = selectedThread?.let { thread ->
        context.getString(
            R.string.workspace_thread_summary,
            thread.tid,
            hex64(thread.userPc),
            hex64(thread.userSp),
        )
    } ?: context.getString(R.string.thread_empty)
    val threadSecondary = selectedThreadRegisters?.let { registers ->
        context.getString(
            R.string.workspace_thread_register_summary,
            hex64(registers.regs.getOrElse(0) { 0uL }),
            hex64(registers.regs.getOrElse(1) { 0uL }),
            hex64(registers.pc),
        )
    } ?: selectedThread?.comm ?: context.getString(R.string.thread_empty)
    val eventPrimary = context.getString(
        R.string.workspace_event_summary,
        snapshot.eventQueueDepth.toInt(),
        recentEvents.size,
    )
    val eventSecondary = latestEvent?.let { entry ->
        context.getString(
            R.string.workspace_event_detail_summary,
            entry.record.code.toString(),
            entry.record.tid,
            entry.record.seq.toString(),
        )
    } ?: context.getString(R.string.event_empty)

    NativeWorkspaceSnapshot(
        expanded = expanded,
        busy = busy,
        connected = snapshot.connected,
        sessionOpen = snapshot.sessionOpen,
        hookActive = snapshot.hookActive,
        targetPid = snapshot.targetPid,
        targetTid = snapshot.targetTid,
        eventQueueDepth = snapshot.eventQueueDepth.toInt(),
        processCount = processes.size,
        threadCount = threads.size,
        eventCount = recentEvents.size,
        imageCount = images.size,
        vmaCount = vmas.size,
        sessionPrimary = sessionPrimary,
        sessionSecondary = sessionSecondary,
        processPrimary = processPrimary,
        processSecondary = processSecondary,
        memoryPrimary = memoryPrimary,
        memorySecondary = memorySecondary,
        threadPrimary = threadPrimary,
        threadSecondary = threadSecondary,
        eventPrimary = eventPrimary,
        eventSecondary = eventSecondary,
        footerMessage = lastMessage,
    )
}
