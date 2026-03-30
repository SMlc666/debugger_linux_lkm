package com.smlc666.lkmdbg.nativeui

import com.smlc666.lkmdbg.data.SessionBridgeState

internal data class NativeWorkspaceSnapshot(
    val expanded: Boolean,
    val connected: Boolean,
    val sessionOpen: Boolean,
    val hookActive: Int,
    val processCount: Int,
    val threadCount: Int,
    val eventCount: Int,
)

internal fun SessionBridgeState.toNativeWorkspaceSnapshot(expanded: Boolean): NativeWorkspaceSnapshot =
    NativeWorkspaceSnapshot(
        expanded = expanded,
        connected = snapshot.connected,
        sessionOpen = snapshot.sessionOpen,
        hookActive = snapshot.hookActive,
        processCount = processes.size,
        threadCount = threads.size,
        eventCount = recentEvents.size,
    )
