package com.smlc666.lkmdbg.overlay.ui.screens

import androidx.annotation.StringRes
import com.smlc666.lkmdbg.appdata.R
import com.smlc666.lkmdbg.data.SessionEventEntry
import com.smlc666.lkmdbg.shared.BridgeEventRecord
import com.smlc666.lkmdbg.shared.BridgeHwpointRecord
import com.smlc666.lkmdbg.shared.BridgeThreadRegistersReply

internal const val THREAD_FLAG_GROUP_LEADER = 0x00000001u
internal const val THREAD_FLAG_SESSION_TARGET = 0x00000002u
internal const val THREAD_FLAG_FREEZE_TRACKED = 0x00000004u
internal const val THREAD_FLAG_FREEZE_SETTLED = 0x00000008u
internal const val THREAD_FLAG_FREEZE_PARKED = 0x00000010u
internal const val THREAD_FLAG_EXITING = 0x00000020u

internal const val STOP_FLAG_ACTIVE = 0x00000001u
internal const val STOP_FLAG_FROZEN = 0x00000002u
internal const val STOP_FLAG_REARM_REQUIRED = 0x00000008u
internal const val STOP_FLAG_SYSCALL_CONTROL = 0x00000020u

internal const val HWPOINT_TYPE_EXEC = 0x00000004u
internal const val HWPOINT_TYPE_WRITE = 0x00000002u
internal const val HWPOINT_FLAG_MMU = 0x00000002u

private const val EVENT_TARGET_SIGNAL = 35u
private const val EVENT_TARGET_STOP = 36u
private const val EVENT_TARGET_SYSCALL = 37u
private const val EVENT_TARGET_SYSCALL_RULE = 41u
private const val EVENT_TARGET_SYSCALL_RULE_DETAIL = 42u

internal enum class EventFilterPreset(@StringRes val labelRes: Int) {
    All(R.string.event_filter_all),
    Pinned(R.string.event_filter_pinned),
    Stops(R.string.event_filter_stops),
    Signals(R.string.event_filter_signals),
    Syscalls(R.string.event_filter_syscalls),
}

internal data class RegisterField(
    val label: String,
    val value: ULong,
)

internal data class RegisterGroup(
    @StringRes val titleRes: Int,
    val fields: List<RegisterField>,
)

internal fun stopReasonLabel(reason: UInt): String = when (reason.toInt()) {
    1 -> "freeze"
    2 -> "breakpoint"
    3 -> "watchpoint"
    4 -> "single-step"
    5 -> "signal"
    6 -> "syscall"
    7 -> "remote-call"
    else -> reason.toString()
}

internal fun stopFlagsLabel(flags: UInt): String {
    val labels = buildList {
        if ((flags and STOP_FLAG_ACTIVE) != 0u) add("active")
        if ((flags and STOP_FLAG_FROZEN) != 0u) add("frozen")
        if ((flags and STOP_FLAG_REARM_REQUIRED) != 0u) add("rearm")
        if ((flags and STOP_FLAG_SYSCALL_CONTROL) != 0u) add("syscall-ctl")
    }
    return if (labels.isEmpty()) "0" else labels.joinToString("+")
}

internal fun hwpointTypeLabel(hwpoint: BridgeHwpointRecord): String = buildString {
    append(
        when (hwpoint.type) {
            HWPOINT_TYPE_EXEC -> "exec"
            HWPOINT_TYPE_WRITE -> "write"
            else -> hwpoint.type.toString()
        },
    )
    if ((hwpoint.flags and HWPOINT_FLAG_MMU) != 0u) {
        append("/mmu")
    } else {
        append("/hw")
    }
}

@StringRes
internal fun threadPrimaryStateLabelRes(flags: UInt): Int = when {
    (flags and THREAD_FLAG_EXITING) != 0u -> R.string.thread_state_exiting
    (flags and THREAD_FLAG_FREEZE_PARKED) != 0u -> R.string.thread_state_stopped
    (flags and (THREAD_FLAG_FREEZE_TRACKED or THREAD_FLAG_FREEZE_SETTLED)) != 0u -> R.string.thread_state_freezing
    else -> R.string.thread_state_running
}

internal fun threadFlagLabelRes(flags: UInt): List<Int> = buildList {
    if ((flags and THREAD_FLAG_GROUP_LEADER) != 0u) add(R.string.thread_flag_group_leader)
    if ((flags and THREAD_FLAG_SESSION_TARGET) != 0u) add(R.string.thread_flag_session_target)
    if ((flags and THREAD_FLAG_FREEZE_TRACKED) != 0u) add(R.string.thread_flag_freeze_tracked)
    if ((flags and THREAD_FLAG_FREEZE_SETTLED) != 0u) add(R.string.thread_flag_freeze_settled)
    if ((flags and THREAD_FLAG_FREEZE_PARKED) != 0u) add(R.string.thread_flag_freeze_parked)
    if ((flags and THREAD_FLAG_EXITING) != 0u) add(R.string.thread_flag_exiting)
}

internal fun buildRegisterGroups(registers: BridgeThreadRegistersReply): List<RegisterGroup> = listOf(
    RegisterGroup(
        titleRes = R.string.thread_regs_group_args,
        fields = (0..7).map { index -> RegisterField("x$index", registers.regs[index]) },
    ),
    RegisterGroup(
        titleRes = R.string.thread_regs_group_temporaries,
        fields = (8..18).map { index -> RegisterField("x$index", registers.regs[index]) },
    ),
    RegisterGroup(
        titleRes = R.string.thread_regs_group_saved,
        fields = (19..28).map { index -> RegisterField("x$index", registers.regs[index]) },
    ),
    RegisterGroup(
        titleRes = R.string.thread_regs_group_frame,
        fields = listOf(
            RegisterField("x29/fp", registers.regs[29]),
            RegisterField("x30/lr", registers.regs[30]),
            RegisterField("sp", registers.sp),
            RegisterField("pc", registers.pc),
        ),
    ),
    RegisterGroup(
        titleRes = R.string.thread_regs_group_fp,
        fields = listOf(
            RegisterField("pstate", registers.pstate),
            RegisterField("features", registers.features.toULong()),
            RegisterField("fpsr", registers.fpsr.toULong()),
            RegisterField("fpcr", registers.fpcr.toULong()),
            RegisterField("v0.lo", registers.v0Lo),
            RegisterField("v0.hi", registers.v0Hi),
        ),
    ),
)

internal fun eventTypeLabel(type: UInt): String = when (type) {
    1u -> "session-opened"
    2u -> "session-reset"
    3u -> "internal-notice"
    16u -> "hook-installed"
    17u -> "hook-removed"
    18u -> "hook-hit"
    32u -> "target-clone"
    33u -> "target-exec"
    34u -> "target-exit"
    EVENT_TARGET_SIGNAL -> "target-signal"
    EVENT_TARGET_STOP -> "target-stop"
    EVENT_TARGET_SYSCALL -> "target-syscall"
    38u -> "target-mmap"
    39u -> "target-munmap"
    40u -> "target-mprotect"
    EVENT_TARGET_SYSCALL_RULE -> "syscall-rule"
    EVENT_TARGET_SYSCALL_RULE_DETAIL -> "syscall-rule-detail"
    else -> "event-$type"
}

internal fun eventCodeLabel(record: BridgeEventRecord): String = when (record.type) {
    EVENT_TARGET_STOP -> stopReasonLabel(record.code)
    EVENT_TARGET_SIGNAL -> "sig=${record.code}"
    EVENT_TARGET_SYSCALL -> "nr=${record.code}"
    EVENT_TARGET_SYSCALL_RULE -> "rule=${record.code}"
    EVENT_TARGET_SYSCALL_RULE_DETAIL -> "detail=${record.code}"
    else -> "code=${record.code}"
}

internal fun eventMatchesPreset(
    entry: SessionEventEntry,
    preset: EventFilterPreset,
    pinnedEventSeqs: Set<ULong>,
): Boolean = when (preset) {
    EventFilterPreset.All -> true
    EventFilterPreset.Pinned -> entry.record.seq in pinnedEventSeqs
    EventFilterPreset.Stops -> entry.record.type == EVENT_TARGET_STOP
    EventFilterPreset.Signals -> entry.record.type == EVENT_TARGET_SIGNAL
    EventFilterPreset.Syscalls -> {
        entry.record.type == EVENT_TARGET_SYSCALL ||
            entry.record.type == EVENT_TARGET_SYSCALL_RULE ||
            entry.record.type == EVENT_TARGET_SYSCALL_RULE_DETAIL
    }
}

internal fun sortEvents(
    events: List<SessionEventEntry>,
    pinnedEventSeqs: Set<ULong>,
): List<SessionEventEntry> = events.sortedWith { left, right ->
    val leftPinned = left.record.seq in pinnedEventSeqs
    val rightPinned = right.record.seq in pinnedEventSeqs
    when {
        leftPinned != rightPinned -> if (leftPinned) -1 else 1
        left.record.seq != right.record.seq -> right.record.seq.compareTo(left.record.seq)
        else -> right.receivedAtMs.compareTo(left.receivedAtMs)
    }
}

internal fun eventMatchesFilter(entry: SessionEventEntry, filterText: String): Boolean {
    if (filterText.isBlank())
        return true
    val record = entry.record
    val needle = filterText.trim().lowercase()
    return listOf(
        record.seq.toString(),
        eventTypeLabel(record.type),
        eventCodeLabel(record),
        record.type.toString(),
        record.code.toString(),
        record.tid.toString(),
        record.tgid.toString(),
        hex32(record.flags),
        hex64(record.sessionId),
        hex64(record.value0),
        hex64(record.value1),
    ).any { it.lowercase().contains(needle) }
}

private fun hex64(value: ULong): String = "0x${value.toString(16)}"

private fun hex32(value: UInt): String = "0x${value.toString(16)}"
