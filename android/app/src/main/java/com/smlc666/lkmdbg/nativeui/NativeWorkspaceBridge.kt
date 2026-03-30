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
    external fun nativeRender(handle: Long)
}
