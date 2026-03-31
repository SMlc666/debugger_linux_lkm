package com.smlc666.lkmdbg.nativeui

import android.content.Context
import android.graphics.SurfaceTexture
import android.view.Choreographer
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.Surface
import android.view.TextureView

internal class NativeWorkspaceTextureView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : TextureView(context, attrs), TextureView.SurfaceTextureListener {
    companion object {
        private const val FIELD_SEPARATOR = '\u001f'
    }

    private var nativeHandle: Long = NativeWorkspaceBridge.nativeCreateRenderer()
    private var surface: Surface? = null
    private var density: Float = resources.displayMetrics.density
    private var renderLoopRunning = false
    private val localizedStrings = loadNativeWorkspaceStrings(context)
    private val frameCallback = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            if (!renderLoopRunning)
                return
            if (nativeHandle != 0L && surface != null && isAvailable) {
                NativeWorkspaceBridge.nativeRender(nativeHandle)
                deliverPendingActions()
            }
            Choreographer.getInstance().postFrameCallback(this)
        }
    }
    private var currentSnapshot = NativeWorkspaceSnapshot(
        expanded = false,
        busy = false,
        connected = false,
        sessionOpen = false,
        hookActive = 0,
        selectedSection = 2,
        targetPid = 0,
        targetTid = 0,
        eventQueueDepth = 0,
        processCount = 0,
        threadCount = 0,
        eventCount = 0,
        imageCount = 0,
        vmaCount = 0,
        sessionPrimary = "",
        sessionSecondary = "",
        processPrimary = "",
        processSecondary = "",
        memoryPrimary = "",
        memorySecondary = "",
        threadPrimary = "",
        threadSecondary = "",
        eventPrimary = "",
        eventSecondary = "",
        processActionChips = emptyList(),
        processEntries = emptyList(),
        memoryActionChips = emptyList(),
        memoryPageActionChips = emptyList(),
        memoryResultEntries = emptyList(),
        memoryPageEntries = emptyList(),
        memoryScalarEntries = emptyList(),
        footerMessage = "",
    )
    private var onWorkspaceAction: ((NativeWorkspaceAction) -> Unit)? = null

    init {
        isOpaque = false
        surfaceTextureListener = this
        pushStaticResources()
    }

    private fun pushStaticResources() {
        NativeWorkspaceBridge.nativeUpdateStrings(
            nativeHandle,
            localizedStrings.title,
            localizedStrings.session,
            localizedStrings.processes,
            localizedStrings.memory,
            localizedStrings.threads,
            localizedStrings.events,
            localizedStrings.sessionSubtitle,
            localizedStrings.processesSubtitle,
            localizedStrings.memorySubtitle,
            localizedStrings.threadsSubtitle,
            localizedStrings.eventsSubtitle,
            localizedStrings.connected,
            localizedStrings.sessionOpen,
            localizedStrings.hook,
            localizedStrings.processCount,
            localizedStrings.threadCount,
            localizedStrings.eventCount,
            localizedStrings.boolYes,
            localizedStrings.boolNo,
        )
        NativeWorkspaceBridge.nativeUpdateFontPaths(
            nativeHandle,
            NativeFontCatalog.buildCandidatePaths(context),
        )
    }

    fun updateSnapshot(snapshot: NativeWorkspaceSnapshot) {
        currentSnapshot = snapshot
        if (nativeHandle == 0L)
            return
        NativeWorkspaceBridge.nativeUpdateState(
            nativeHandle,
            snapshot.expanded,
            snapshot.busy,
            snapshot.selectedSection,
            snapshot.connected,
            snapshot.sessionOpen,
            snapshot.hookActive,
            snapshot.targetPid,
            snapshot.targetTid,
            snapshot.eventQueueDepth,
            snapshot.processCount,
            snapshot.threadCount,
            snapshot.eventCount,
            snapshot.imageCount,
            snapshot.vmaCount,
            snapshot.sessionPrimary,
            snapshot.sessionSecondary,
            snapshot.processPrimary,
            snapshot.processSecondary,
            snapshot.memoryPrimary,
            snapshot.memorySecondary,
            snapshot.threadPrimary,
            snapshot.threadSecondary,
            snapshot.eventPrimary,
            snapshot.eventSecondary,
            snapshot.processActionChips.map(::serializeActionChip).toTypedArray(),
            snapshot.processEntries.map(::serializeEntry).toTypedArray(),
            snapshot.memoryActionChips.map(::serializeActionChip).toTypedArray(),
            snapshot.memoryPageActionChips.map(::serializeActionChip).toTypedArray(),
            snapshot.memoryResultEntries.map(::serializeEntry).toTypedArray(),
            snapshot.memoryPageEntries.map(::serializeEntry).toTypedArray(),
            snapshot.memoryScalarEntries.toTypedArray(),
            snapshot.footerMessage,
        )
        requestRender()
    }

    fun setOnWorkspaceActionListener(listener: ((NativeWorkspaceAction) -> Unit)?) {
        onWorkspaceAction = listener
    }

    fun requestRender() {
        if (nativeHandle == 0L || surface == null || !isAvailable)
            return
        post {
            if (nativeHandle != 0L && surface != null && isAvailable) {
                NativeWorkspaceBridge.nativeRender(nativeHandle)
                deliverPendingActions()
            }
        }
    }

    override fun onSurfaceTextureAvailable(surfaceTexture: SurfaceTexture, width: Int, height: Int) {
        surface = Surface(surfaceTexture)
        if (nativeHandle == 0L)
            nativeHandle = NativeWorkspaceBridge.nativeCreateRenderer()
        pushStaticResources()
        NativeWorkspaceBridge.nativeSetSurface(nativeHandle, surface)
        NativeWorkspaceBridge.nativeResize(nativeHandle, width, height, density)
        startRenderLoop()
        updateSnapshot(currentSnapshot)
    }

    override fun onSurfaceTextureSizeChanged(surfaceTexture: SurfaceTexture, width: Int, height: Int) {
        if (nativeHandle == 0L)
            return
        NativeWorkspaceBridge.nativeResize(nativeHandle, width, height, density)
        requestRender()
    }

    override fun onSurfaceTextureDestroyed(surfaceTexture: SurfaceTexture): Boolean {
        stopRenderLoop()
        if (nativeHandle != 0L)
            NativeWorkspaceBridge.nativeSetSurface(nativeHandle, null)
        surface?.release()
        surface = null
        return true
    }

    override fun onSurfaceTextureUpdated(surfaceTexture: SurfaceTexture) = Unit

    override fun onDetachedFromWindow() {
        stopRenderLoop()
        if (nativeHandle != 0L) {
            NativeWorkspaceBridge.nativeDestroyRenderer(nativeHandle)
            nativeHandle = 0L
        }
        surface?.release()
        surface = null
        super.onDetachedFromWindow()
    }

    fun dispatchNativeTouch(event: MotionEvent): Boolean {
        if (nativeHandle == 0L)
            return false
        NativeWorkspaceBridge.nativeOnTouch(nativeHandle, event.actionMasked, event.x, event.y)
        requestRender()
        return true
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        if (surface != null && isAvailable)
            startRenderLoop()
    }

    private fun startRenderLoop() {
        if (renderLoopRunning)
            return
        renderLoopRunning = true
        Choreographer.getInstance().postFrameCallback(frameCallback)
    }

    private fun stopRenderLoop() {
        if (!renderLoopRunning)
            return
        renderLoopRunning = false
        Choreographer.getInstance().removeFrameCallback(frameCallback)
    }

    private fun deliverPendingActions() {
        val listener = onWorkspaceAction ?: return
        while (true) {
            val raw = NativeWorkspaceBridge.nativeConsumeAction(nativeHandle) ?: break
            val action = NativeWorkspaceAction.parse(raw) ?: continue
            listener(action)
        }
    }

    private fun serializeActionChip(chip: NativeWorkspaceActionChipSnapshot): String =
        listOf(
            chip.actionKey,
            chip.label,
            if (chip.active) "1" else "0",
        ).joinToString(FIELD_SEPARATOR.toString())

    private fun serializeEntry(entry: NativeWorkspaceListEntrySnapshot): String =
        listOf(
            entry.actionKey,
            entry.title,
            entry.subtitle,
            entry.badge,
            if (entry.selected) "1" else "0",
        ).joinToString(FIELD_SEPARATOR.toString())
}
