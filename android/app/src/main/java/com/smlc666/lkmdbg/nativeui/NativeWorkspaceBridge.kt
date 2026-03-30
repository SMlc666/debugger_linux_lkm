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
        connected: Boolean,
        sessionOpen: Boolean,
        hookActive: Int,
        processCount: Int,
        threadCount: Int,
        eventCount: Int,
    )
    external fun nativeUpdateStrings(
        handle: Long,
        title: String,
        session: String,
        processes: String,
        memory: String,
        threads: String,
        events: String,
        connected: String,
        sessionOpen: String,
        hook: String,
        processCount: String,
        threadCount: String,
        eventCount: String,
    )
    external fun nativeUpdateFontPaths(handle: Long, fontPaths: Array<String>)
    external fun nativeOnTouch(handle: Long, action: Int, x: Float, y: Float)
    external fun nativeRender(handle: Long)
}
