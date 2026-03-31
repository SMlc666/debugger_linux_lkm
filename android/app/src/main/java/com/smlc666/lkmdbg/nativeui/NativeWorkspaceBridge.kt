package com.smlc666.lkmdbg.nativeui

import android.view.Surface

internal object NativeWorkspaceBridge {
    init {
        System.loadLibrary("lkmdbg_native")
    }

    external fun nativeCreateRenderer(): Long
    external fun nativeDestroyRenderer(handle: Long)
    external fun nativeSetSurface(handle: Long, surface: Surface?)
    external fun nativeResize(handle: Long, width: Int, height: Int, density: Float)
    external fun nativeUpdateState(
        handle: Long,
        expanded: Boolean,
        busy: Boolean,
        connected: Boolean,
        sessionOpen: Boolean,
        hookActive: Int,
        targetPid: Int,
        targetTid: Int,
        eventQueueDepth: Int,
        processCount: Int,
        threadCount: Int,
        eventCount: Int,
        imageCount: Int,
        vmaCount: Int,
        sessionPrimary: String,
        sessionSecondary: String,
        processPrimary: String,
        processSecondary: String,
        memoryPrimary: String,
        memorySecondary: String,
        threadPrimary: String,
        threadSecondary: String,
        eventPrimary: String,
        eventSecondary: String,
        footerMessage: String,
    )
    external fun nativeUpdateStrings(
        handle: Long,
        title: String,
        session: String,
        processes: String,
        memory: String,
        threads: String,
        events: String,
        sessionSubtitle: String,
        processesSubtitle: String,
        memorySubtitle: String,
        threadsSubtitle: String,
        eventsSubtitle: String,
        connected: String,
        sessionOpen: String,
        hook: String,
        processCount: String,
        threadCount: String,
        eventCount: String,
        boolYes: String,
        boolNo: String,
    )
    external fun nativeUpdateFontPaths(handle: Long, fontPaths: Array<String>)
    external fun nativeOnTouch(handle: Long, action: Int, x: Float, y: Float)
    external fun nativeRender(handle: Long)
}
