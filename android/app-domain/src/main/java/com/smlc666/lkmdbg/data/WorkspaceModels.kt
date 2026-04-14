package com.smlc666.lkmdbg.data

enum class WorkspaceSection {
    Session,
    Processes,
    Memory,
    Threads,
    Events,
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
