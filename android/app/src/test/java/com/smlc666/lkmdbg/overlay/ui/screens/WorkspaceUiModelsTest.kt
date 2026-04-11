package com.smlc666.lkmdbg.overlay.ui.screens

import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.data.SessionEventEntry
import com.smlc666.lkmdbg.shared.BridgeEventRecord
import com.smlc666.lkmdbg.shared.BridgeThreadRegistersReply
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class WorkspaceUiModelsTest {
    @Test
    fun eventMatchesFilter_matchesDerivedTypeAndCodeLabels() {
        val entry = eventEntry(
            type = 36u,
            code = 3u,
            seq = 17uL,
            tid = 4242,
        )

        assertTrue(eventMatchesFilter(entry, "target-stop"))
        assertTrue(eventMatchesFilter(entry, "watchpoint"))
        assertTrue(eventMatchesFilter(entry, "4242"))
        assertFalse(eventMatchesFilter(entry, "syscall-rule"))
    }

    @Test
    fun eventMatchesPreset_respectsPinnedAndTypeBuckets() {
        val pinned = setOf(7uL)
        val stopEntry = eventEntry(type = 36u, code = 1u, seq = 7uL)
        val signalEntry = eventEntry(type = 35u, code = 9u, seq = 8uL)
        val syscallEntry = eventEntry(type = 37u, code = 64u, seq = 9uL)

        assertTrue(eventMatchesPreset(stopEntry, EventFilterPreset.Pinned, pinned))
        assertTrue(eventMatchesPreset(stopEntry, EventFilterPreset.Stops, pinned))
        assertTrue(eventMatchesPreset(signalEntry, EventFilterPreset.Signals, pinned))
        assertTrue(eventMatchesPreset(syscallEntry, EventFilterPreset.Syscalls, pinned))
        assertFalse(eventMatchesPreset(signalEntry, EventFilterPreset.Stops, pinned))
        assertFalse(eventMatchesPreset(stopEntry, EventFilterPreset.Syscalls, pinned))
    }

    @Test
    fun sortEvents_keepsPinnedEntriesFirstThenNewestSeq() {
        val olderPinned = eventEntry(type = 36u, code = 1u, seq = 5uL, receivedAtMs = 10L)
        val newerUnpinned = eventEntry(type = 35u, code = 9u, seq = 12uL, receivedAtMs = 20L)
        val newerPinned = eventEntry(type = 37u, code = 64u, seq = 9uL, receivedAtMs = 30L)

        val sorted = sortEvents(
            events = listOf(olderPinned, newerUnpinned, newerPinned),
            pinnedEventSeqs = setOf(5uL, 9uL),
        )

        assertEquals(listOf(9uL, 5uL, 12uL), sorted.map { it.record.seq })
    }

    @Test
    fun threadPrimaryStateLabelRes_prefersExitingOverFreezeFlags() {
        assertEquals(R.string.thread_state_exiting, threadPrimaryStateLabelRes(THREAD_FLAG_EXITING))
        assertEquals(R.string.thread_state_stopped, threadPrimaryStateLabelRes(THREAD_FLAG_FREEZE_PARKED))
        assertEquals(
            R.string.thread_state_freezing,
            threadPrimaryStateLabelRes(THREAD_FLAG_FREEZE_TRACKED or THREAD_FLAG_FREEZE_SETTLED),
        )
        assertEquals(R.string.thread_state_running, threadPrimaryStateLabelRes(0u))
    }

    @Test
    fun buildRegisterGroups_emitsExpectedSectionsAndFields() {
        val registers = BridgeThreadRegistersReply(
            status = 0,
            tid = 77,
            flags = 0u,
            regs = ULongArray(31) { index -> index.toULong() },
            sp = 0x1000uL,
            pc = 0x2000uL,
            pstate = 0x3000uL,
            features = 0x4u,
            fpsr = 0x5u,
            fpcr = 0x6u,
            v0Lo = 0x7uL,
            v0Hi = 0x8uL,
            message = "",
        )

        val groups = buildRegisterGroups(registers)

        assertEquals(
            listOf(
                R.string.thread_regs_group_args,
                R.string.thread_regs_group_temporaries,
                R.string.thread_regs_group_saved,
                R.string.thread_regs_group_frame,
                R.string.thread_regs_group_fp,
            ),
            groups.map { it.titleRes },
        )
        assertEquals("x0", groups.first().fields.first().label)
        assertEquals("pc", groups[3].fields.last().label)
        assertEquals(0x8uL, groups.last().fields.last().value)
    }

    private fun eventEntry(
        type: UInt,
        code: UInt,
        seq: ULong,
        tid: Int = 100,
        receivedAtMs: Long = 1L,
    ) = SessionEventEntry(
        record = BridgeEventRecord(
            version = 3u,
            type = type,
            size = 0u,
            code = code,
            sessionId = 1uL,
            seq = seq,
            tgid = 10,
            tid = tid,
            flags = 0x20u,
            value0 = 0x1234uL,
            value1 = 0x5678uL,
        ),
        receivedAtMs = receivedAtMs,
    )
}
