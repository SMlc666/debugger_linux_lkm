package com.smlc666.lkmdbg.overlay

import android.content.Context
import android.content.Intent
import android.graphics.PixelFormat
import android.os.IBinder
import android.os.Bundle
import android.view.Gravity
import android.view.WindowManager
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.platform.ViewCompositionStrategy
import androidx.lifecycle.LifecycleService
import androidx.lifecycle.ViewModelStore
import androidx.lifecycle.ViewModelStoreOwner
import androidx.lifecycle.ViewTreeLifecycleOwner
import androidx.lifecycle.ViewTreeViewModelStoreOwner
import androidx.savedstate.SavedStateRegistry
import androidx.savedstate.SavedStateRegistryController
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.ViewTreeSavedStateRegistryOwner
import com.smlc666.lkmdbg.CrashLogger
import com.smlc666.lkmdbg.LkmdbgApplication
import com.smlc666.lkmdbg.data.SessionBridgeRepository
import com.smlc666.lkmdbg.ui.theme.LkmdbgTheme
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.roundToInt

class LkmdbgOverlayService : LifecycleService() {
    private val overlayViewTreeOwner = OverlayViewTreeOwner()
    private lateinit var windowManager: WindowManager
    private lateinit var repository: SessionBridgeRepository
    private var rootView: ComposeView? = null
    private var layoutParams: WindowManager.LayoutParams? = null
    private var collapsedDiameterPx = 0
    private var collapsedX = 0
    private var collapsedY = 0
    private var expanded by mutableStateOf(false)

    override fun onBind(intent: Intent): IBinder? = super.onBind(intent)

    override fun onCreate() {
        super.onCreate()
        running.set(true)
        try {
            overlayViewTreeOwner.attach()
            repository = (application as LkmdbgApplication).sessionRepository
            windowManager = getSystemService(WINDOW_SERVICE) as WindowManager
            createOverlay()
        } catch (t: Throwable) {
            CrashLogger.logHandled(this, "overlay_on_create", t)
            running.set(false)
            stopSelf()
            throw t
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopSelf()
            return START_NOT_STICKY
        }
        return START_STICKY
    }

    override fun onDestroy() {
        removeOverlay()
        overlayViewTreeOwner.dispose()
        running.set(false)
        super.onDestroy()
    }

    private fun createOverlay() {
        if (rootView != null)
            return

        val density = resources.displayMetrics.density
        collapsedDiameterPx = (72f * density).roundToInt()
        collapsedX = (18f * density).roundToInt()
        collapsedY = (96f * density).roundToInt()

        val params = WindowManager.LayoutParams(
            collapsedDiameterPx,
            collapsedDiameterPx,
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN or
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE,
            PixelFormat.TRANSLUCENT,
        ).apply {
            gravity = Gravity.TOP or Gravity.START
            x = collapsedX
            y = collapsedY
        }

        val composeView = ComposeView(this).apply {
            setViewCompositionStrategy(ViewCompositionStrategy.DisposeOnDetachedFromWindow)
            ViewTreeLifecycleOwner.set(this, this@LkmdbgOverlayService)
            ViewTreeViewModelStoreOwner.set(this, overlayViewTreeOwner)
            ViewTreeSavedStateRegistryOwner.set(this, overlayViewTreeOwner)
            setContent {
                LkmdbgTheme {
                    OverlayWorkspace(
                        repository = repository,
                        expanded = expanded,
                        onExpand = { updateExpandedState(true) },
                        onCollapse = { updateExpandedState(false) },
                        onMoveBallBy = { dx, dy -> moveCollapsedBall(dx, dy) },
                        onClose = { stopSelf() },
                    )
                }
            }
        }

        rootView = composeView
        layoutParams = params
        windowManager.addView(composeView, params)
    }

    private fun moveCollapsedBall(deltaX: Float, deltaY: Float) {
        if (expanded)
            return
        collapsedX += deltaX.roundToInt()
        collapsedY += deltaY.roundToInt()
        val view = rootView ?: return
        val params = layoutParams ?: return
        params.x = collapsedX
        params.y = collapsedY
        windowManager.updateViewLayout(view, params)
    }

    private fun updateExpandedState(nextExpanded: Boolean) {
        if (expanded == nextExpanded)
            return
        expanded = nextExpanded
        val params = layoutParams ?: return
        val view = rootView ?: return
        if (nextExpanded) {
            params.width = WindowManager.LayoutParams.MATCH_PARENT
            params.height = WindowManager.LayoutParams.MATCH_PARENT
            params.x = 0
            params.y = 0
        } else {
            params.width = collapsedDiameterPx
            params.height = collapsedDiameterPx
            params.x = collapsedX
            params.y = collapsedY
        }
        windowManager.updateViewLayout(view, params)
    }

    private fun removeOverlay() {
        rootView?.let { view ->
            runCatching { windowManager.removeView(view) }
        }
        rootView = null
        layoutParams = null
    }

    companion object {
        private const val ACTION_SHOW = "com.smlc666.lkmdbg.overlay.SHOW"
        private const val ACTION_STOP = "com.smlc666.lkmdbg.overlay.STOP"
        private val running = AtomicBoolean(false)

        fun isRunning(): Boolean = running.get()

        fun start(context: Context) {
            if (!OverlayPermission.hasPermission(context))
                return
            val intent = Intent(context, LkmdbgOverlayService::class.java).setAction(ACTION_SHOW)
            runCatching {
                context.startService(intent)
            }.onFailure { throwable ->
                CrashLogger.logHandled(context, "overlay_start", throwable)
            }
        }

        fun stop(context: Context) {
            val intent = Intent(context, LkmdbgOverlayService::class.java).setAction(ACTION_STOP)
            runCatching {
                context.startService(intent)
            }.onFailure { throwable ->
                CrashLogger.logHandled(context, "overlay_stop", throwable)
            }
        }
    }

    private inner class OverlayViewTreeOwner : SavedStateRegistryOwner, ViewModelStoreOwner {
        private val store = ViewModelStore()
        private val controller = SavedStateRegistryController.create(this)
        private var attached = false

        override val lifecycle
            get() = this@LkmdbgOverlayService.lifecycle

        override val savedStateRegistry: SavedStateRegistry
            get() = controller.savedStateRegistry

        override val viewModelStore: ViewModelStore
            get() = store

        fun attach() {
            if (attached)
                return
            controller.performAttach()
            controller.performRestore(Bundle.EMPTY)
            attached = true
        }

        fun dispose() {
            if (!attached)
                return
            controller.performSave(Bundle())
            store.clear()
            attached = false
        }
    }
}
