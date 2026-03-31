package com.smlc666.lkmdbg.nativeui

import android.content.Context
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.data.MemorySearchRefineMode
import com.smlc666.lkmdbg.data.ProcessFilter
import com.smlc666.lkmdbg.data.ResolvedProcessRecord
import com.smlc666.lkmdbg.data.SessionBridgeState

internal data class NativeWorkspaceActionChipSnapshot(
    val actionKey: String,
    val label: String,
    val active: Boolean = false,
)

internal data class NativeWorkspaceListEntrySnapshot(
    val actionKey: String,
    val title: String,
    val subtitle: String = "",
    val badge: String = "",
    val selected: Boolean = false,
)

internal data class NativeWorkspaceSnapshot(
    val expanded: Boolean,
    val busy: Boolean,
    val selectedSection: Int,
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
    val processActionChips: List<NativeWorkspaceActionChipSnapshot>,
    val processEntries: List<NativeWorkspaceListEntrySnapshot>,
    val memoryActionChips: List<NativeWorkspaceActionChipSnapshot>,
    val memoryPageActionChips: List<NativeWorkspaceActionChipSnapshot>,
    val memoryResultEntries: List<NativeWorkspaceListEntrySnapshot>,
    val memoryPageEntries: List<NativeWorkspaceListEntrySnapshot>,
    val memoryScalarEntries: List<String>,
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

private fun processKindLabel(context: Context, process: ResolvedProcessRecord): String =
    when {
        process.isAndroidApp && process.isSystemApp -> context.getString(R.string.process_kind_system_app)
        process.isAndroidApp -> context.getString(R.string.process_kind_user_app)
        else -> context.getString(R.string.process_kind_command_line)
    }

private fun sanitizeUiText(value: String): String =
    value.replace('\n', ' ').replace('\r', ' ').trim()

internal fun SessionBridgeState.toNativeWorkspaceSnapshot(
    context: Context,
    expanded: Boolean,
): NativeWorkspaceSnapshot {
    val selectedThread = selectedThreadTid?.let { tid -> threads.firstOrNull { it.tid == tid } }
        ?: threads.firstOrNull()
    val latestEvent = recentEvents.firstOrNull()
    val page = memoryPage
    val filteredProcesses = processes.filter { processFilter.matches(it) }
    val firstProcess = filteredProcesses.firstOrNull() ?: processes.firstOrNull()
    val focusedRowAddress = page?.focusAddress
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
    val processActionChips = buildList {
        add(
            NativeWorkspaceActionChipSnapshot(
                actionKey = "process:filter_next",
                label = processFilterLabel(context, processFilter),
                active = true,
            ),
        )
        add(
            NativeWorkspaceActionChipSnapshot(
                actionKey = "process:refresh",
                label = context.getString(R.string.process_action_refresh),
            ),
        )
    }
    val processEntries = filteredProcesses.take(12).map { process ->
        NativeWorkspaceListEntrySnapshot(
            actionKey = "process:attach:${process.pid}",
            title = sanitizeUiText(process.displayName),
            subtitle = sanitizeUiText(
                buildString {
                    append("pid=")
                    append(process.pid)
                    append(" uid=")
                    append(process.uid)
                    if (!process.packageName.isNullOrBlank()) {
                        append(" · ")
                        append(process.packageName)
                    }
                },
            ),
            badge = processKindLabel(context, process),
            selected = process.pid == snapshot.targetPid,
        )
    }
    val memoryActionChips = listOf(
        NativeWorkspaceActionChipSnapshot(
            actionKey = "memory:type_next",
            label = context.getString(memorySearch.valueType.labelRes),
            active = true,
        ),
        NativeWorkspaceActionChipSnapshot(
            actionKey = "memory:mode_next",
            label = context.getString(memorySearch.refineMode.labelRes),
            active = memorySearch.refineMode != MemorySearchRefineMode.Exact,
        ),
        NativeWorkspaceActionChipSnapshot(
            actionKey = "memory:region_next",
            label = context.getString(memorySearch.regionPreset.labelRes),
            active = true,
        ),
    )
    val memoryPageActionChips = listOf(
        NativeWorkspaceActionChipSnapshot(
            actionKey = "memory:search",
            label = context.getString(R.string.memory_action_search),
        ),
        NativeWorkspaceActionChipSnapshot(
            actionKey = "memory:refine",
            label = context.getString(R.string.memory_action_refine),
        ),
        NativeWorkspaceActionChipSnapshot(
            actionKey = "memory:prev_page",
            label = context.getString(R.string.memory_action_prev_page),
        ),
        NativeWorkspaceActionChipSnapshot(
            actionKey = "memory:next_page",
            label = context.getString(R.string.memory_action_next_page),
        ),
        NativeWorkspaceActionChipSnapshot(
            actionKey = "memory:preview_pc",
            label = context.getString(R.string.memory_action_preview_pc),
        ),
    )
    val memoryResultEntries = memorySearch.results.take(16).map { result ->
        NativeWorkspaceListEntrySnapshot(
            actionKey = "memory:open:${result.address}",
            title = sanitizeUiText("${hex64(result.address)} · ${result.valueSummary}"),
            subtitle = sanitizeUiText(
                buildString {
                    append(result.regionName.ifBlank { context.getString(R.string.memory_ranges_unnamed) })
                    append(" · ")
                    append(hex64(result.regionStart))
                    append(" - ")
                    append(hex64(result.regionEnd))
                },
            ),
            badge = result.previewHex.take(23),
            selected = page?.focusAddress == result.address,
        )
    }
    val memoryPageEntries = page?.rows?.take(12)?.map { row ->
        val rowEndExclusive = row.address + row.byteValues.size.toUInt().toULong()
        NativeWorkspaceListEntrySnapshot(
            actionKey = "memory:focus:${row.address}",
            title = sanitizeUiText("${hex64(row.address)} · ${row.hexBytes}"),
            subtitle = sanitizeUiText(row.ascii),
            badge = row.byteValues.size.toString(),
            selected = focusedRowAddress != null &&
                focusedRowAddress >= row.address &&
                focusedRowAddress < rowEndExclusive,
        )
    }.orEmpty()
    val memoryScalarEntries = page?.scalars?.map { scalar ->
        sanitizeUiText("${scalar.label}: ${scalar.value}")
    }.orEmpty()
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

    return NativeWorkspaceSnapshot(
        expanded = expanded,
        busy = busy,
        selectedSection = workspaceSection.ordinal,
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
        processActionChips = processActionChips,
        processEntries = processEntries,
        memoryActionChips = memoryActionChips,
        memoryPageActionChips = memoryPageActionChips,
        memoryResultEntries = memoryResultEntries,
        memoryPageEntries = memoryPageEntries,
        memoryScalarEntries = memoryScalarEntries,
        footerMessage = lastMessage,
    )
}
