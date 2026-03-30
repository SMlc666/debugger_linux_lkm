package com.smlc666.lkmdbg.nativeui

import android.content.Context
import android.graphics.SurfaceTexture
import android.util.AttributeSet
import android.view.Surface
import android.view.TextureView

internal class NativeWorkspaceTextureView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : TextureView(context, attrs), TextureView.SurfaceTextureListener {
    private var nativeHandle: Long = NativeWorkspaceBridge.nativeCreateRenderer()
    private var surface: Surface? = null
    private var density: Float = resources.displayMetrics.density
    private var currentSnapshot = NativeWorkspaceSnapshot(
        expanded = false,
        connected = false,
        sessionOpen = false,
        hookActive = 0,
        processCount = 0,
        threadCount = 0,
        eventCount = 0,
    )

    init {
        isOpaque = false
        surfaceTextureListener = this
    }

    fun updateSnapshot(snapshot: NativeWorkspaceSnapshot) {
        currentSnapshot = snapshot
        if (nativeHandle == 0L)
            return
        NativeWorkspaceBridge.nativeUpdateState(
            nativeHandle,
            snapshot.expanded,
            snapshot.connected,
            snapshot.sessionOpen,
            snapshot.hookActive,
            snapshot.processCount,
            snapshot.threadCount,
            snapshot.eventCount,
        )
        requestRender()
    }

    fun requestRender() {
        if (nativeHandle == 0L || surface == null || !isAvailable)
            return
        post {
            if (nativeHandle != 0L && surface != null && isAvailable)
                NativeWorkspaceBridge.nativeRender(nativeHandle)
        }
    }

    override fun onSurfaceTextureAvailable(surfaceTexture: SurfaceTexture, width: Int, height: Int) {
        surface = Surface(surfaceTexture)
        if (nativeHandle == 0L)
            nativeHandle = NativeWorkspaceBridge.nativeCreateRenderer()
        NativeWorkspaceBridge.nativeSetSurface(nativeHandle, surface)
        NativeWorkspaceBridge.nativeResize(nativeHandle, width, height, density)
        updateSnapshot(currentSnapshot)
    }

    override fun onSurfaceTextureSizeChanged(surfaceTexture: SurfaceTexture, width: Int, height: Int) {
        if (nativeHandle == 0L)
            return
        NativeWorkspaceBridge.nativeResize(nativeHandle, width, height, density)
        requestRender()
    }

    override fun onSurfaceTextureDestroyed(surfaceTexture: SurfaceTexture): Boolean {
        if (nativeHandle != 0L)
            NativeWorkspaceBridge.nativeSetSurface(nativeHandle, null)
        surface?.release()
        surface = null
        return true
    }

    override fun onSurfaceTextureUpdated(surfaceTexture: SurfaceTexture) = Unit

    override fun onDetachedFromWindow() {
        if (nativeHandle != 0L) {
            NativeWorkspaceBridge.nativeDestroyRenderer(nativeHandle)
            nativeHandle = 0L
        }
        surface?.release()
        surface = null
        super.onDetachedFromWindow()
    }
}
