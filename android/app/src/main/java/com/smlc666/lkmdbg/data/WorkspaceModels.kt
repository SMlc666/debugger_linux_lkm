package com.smlc666.lkmdbg.data

import com.smlc666.lkmdbg.R

enum class WorkspaceSection(val labelRes: Int, val iconRes: Int) {
    Session(R.string.workspace_session, R.drawable.ic_lkmdbg_terminal),
    Processes(R.string.workspace_processes, R.drawable.ic_lkmdbg_radar),
    Memory(R.string.workspace_memory, R.drawable.ic_lkmdbg_cpu),
    Threads(R.string.workspace_threads, R.drawable.ic_lkmdbg_cpu),
    Events(R.string.workspace_events, R.drawable.ic_lkmdbg_terminal),
    ;

    fun next(): WorkspaceSection {
        val values = entries
        return values[(ordinal + 1) % values.size]
    }

    companion object {
        fun fromOrdinal(value: Int): WorkspaceSection =
            entries.getOrElse(value) { Memory }
    }
}
