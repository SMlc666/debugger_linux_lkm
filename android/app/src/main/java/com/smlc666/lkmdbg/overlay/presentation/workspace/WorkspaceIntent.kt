package com.smlc666.lkmdbg.overlay.presentation.workspace

import com.smlc666.lkmdbg.data.WorkspaceSection

sealed interface WorkspaceIntent {
    open class SelectSection(val section: WorkspaceSection) : WorkspaceIntent {
        override fun equals(other: Any?): Boolean =
            other is SelectSection && section == other.section

        override fun hashCode(): Int = section.hashCode()

        override fun toString(): String = "SelectSection(section=$section)"
    }

    data class SelectThread(val tid: Int) : SelectSection(WorkspaceSection.Threads)
}
