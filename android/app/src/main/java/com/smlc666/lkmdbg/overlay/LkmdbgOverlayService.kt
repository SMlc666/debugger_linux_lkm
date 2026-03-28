package com.smlc666.lkmdbg.overlay

import android.app.Service
import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.graphics.PixelFormat
import android.graphics.drawable.GradientDrawable
import android.os.IBinder
import android.text.Editable
import android.text.TextWatcher
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import androidx.core.view.setPadding
import com.smlc666.lkmdbg.LkmdbgApplication
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.data.SessionBridgeRepository
import com.smlc666.lkmdbg.data.SessionBridgeState
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.roundToInt

class LkmdbgOverlayService : Service() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private lateinit var windowManager: WindowManager
    private lateinit var repository: SessionBridgeRepository
    private var rootView: View? = null
    private var layoutParams: WindowManager.LayoutParams? = null
    private var statusView: TextView? = null
    private var messageView: TextView? = null
    private var pidInput: EditText? = null
    private var collectionJob: Job? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        running.set(true)
        repository = (application as LkmdbgApplication).sessionRepository
        windowManager = getSystemService(WINDOW_SERVICE) as WindowManager
        createOverlay()
        collectionJob = scope.launch {
            repository.state.collectLatest(::renderState)
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
        collectionJob?.cancel()
        removeOverlay()
        scope.cancel()
        running.set(false)
        super.onDestroy()
    }

    private fun createOverlay() {
        if (rootView != null)
            return

        val density = resources.displayMetrics.density
        val card = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = GradientDrawable().apply {
                shape = GradientDrawable.RECTANGLE
                cornerRadius = 24f * density
                setColor(Color.parseColor("#E6121A22"))
                setStroke((1f * density).roundToInt(), Color.parseColor("#335F7A"))
            }
            setPadding((14f * density).roundToInt())
            elevation = 18f * density
        }

        val handle = TextView(this).apply {
            text = getString(R.string.overlay_drag_handle)
            setTextColor(Color.parseColor("#FFB7D4E8"))
            textSize = 12f
            setPadding((6f * density).roundToInt())
            background = GradientDrawable().apply {
                shape = GradientDrawable.RECTANGLE
                cornerRadius = 16f * density
                setColor(Color.parseColor("#22354A"))
            }
        }

        val title = TextView(this).apply {
            text = getString(R.string.overlay_title)
            setTextColor(Color.WHITE)
            textSize = 18f
        }

        statusView = TextView(this).apply {
            setTextColor(Color.parseColor("#FFD6E7F2"))
            textSize = 13f
        }

        messageView = TextView(this).apply {
            setTextColor(Color.parseColor("#FF94B6C7"))
            textSize = 12f
        }

        pidInput = EditText(this).apply {
            hint = getString(R.string.session_target_pid)
            setSingleLine()
            setTextColor(Color.WHITE)
            setHintTextColor(Color.parseColor("#889CB1"))
            background = GradientDrawable().apply {
                shape = GradientDrawable.RECTANGLE
                cornerRadius = 18f * density
                setColor(Color.parseColor("#10212D"))
                setStroke((1f * density).roundToInt(), Color.parseColor("#335F7A"))
            }
            setPadding((12f * density).roundToInt())
            addTextChangedListener(
                object : TextWatcher {
                    override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) = Unit
                    override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) = Unit
                    override fun afterTextChanged(s: Editable?) {
                        repository.updateTargetPidInput(s?.toString().orEmpty())
                    }
                },
            )
        }

        val buttonRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_HORIZONTAL
        }

        val connectButton = makeButton(R.string.session_action_connect) {
            scope.launch { repository.connect() }
        }
        val openButton = makeButton(R.string.session_action_open_session) {
            scope.launch { repository.openSession() }
        }
        val refreshButton = makeButton(R.string.session_action_refresh) {
            scope.launch { repository.refreshStatus() }
        }
        val attachButton = makeButton(R.string.session_action_attach_target) {
            repository.updateTargetPidInput(pidInput?.text?.toString().orEmpty())
            scope.launch { repository.attachTarget() }
        }
        val closeButton = makeButton(R.string.overlay_action_close) {
            stopSelf()
        }

        buttonRow.addView(connectButton, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        buttonRow.addView(openButton, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        buttonRow.addView(refreshButton, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        buttonRow.addView(attachButton, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))

        card.addView(handle, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        card.addView(spacer(density, 10))
        card.addView(title)
        card.addView(spacer(density, 6))
        card.addView(requireNotNull(statusView))
        card.addView(spacer(density, 4))
        card.addView(requireNotNull(messageView))
        card.addView(spacer(density, 10))
        card.addView(requireNotNull(pidInput), LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        card.addView(spacer(density, 10))
        card.addView(buttonRow)
        card.addView(spacer(density, 8))
        card.addView(closeButton, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))

        val params = WindowManager.LayoutParams(
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
            PixelFormat.TRANSLUCENT,
        ).apply {
            gravity = Gravity.TOP or Gravity.START
            x = (18f * density).roundToInt()
            y = (96f * density).roundToInt()
        }

        installDragHandle(handle, params)
        rootView = card
        layoutParams = params
        windowManager.addView(card, params)
    }

    private fun makeButton(textRes: Int, onClick: () -> Unit): Button =
        Button(this).apply {
            text = getString(textRes)
            setOnClickListener { onClick() }
        }

    private fun spacer(density: Float, heightDp: Int): View =
        View(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                (heightDp * density).roundToInt(),
            )
        }

    private fun installDragHandle(handle: View, params: WindowManager.LayoutParams) {
        var startX = 0
        var startY = 0
        var touchX = 0f
        var touchY = 0f

        handle.setOnTouchListener { _, event ->
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    startX = params.x
                    startY = params.y
                    touchX = event.rawX
                    touchY = event.rawY
                    true
                }

                MotionEvent.ACTION_MOVE -> {
                    params.x = startX + (event.rawX - touchX).roundToInt()
                    params.y = startY + (event.rawY - touchY).roundToInt()
                    rootView?.let { windowManager.updateViewLayout(it, params) }
                    true
                }

                else -> false
            }
        }
    }

    private fun renderState(state: SessionBridgeState) {
        statusView?.text = getString(
            R.string.overlay_status_template,
            state.snapshot.transport,
            state.snapshot.targetPid,
            state.snapshot.targetTid,
            state.snapshot.sessionId.toString(),
        )
        messageView?.text = getString(R.string.session_last_message, state.lastMessage)
        val input = pidInput
        if (input != null && !input.isFocused) {
            val nextText = state.targetPidInput
            if (input.text.toString() != nextText)
                input.setText(nextText)
        }
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
            context.startService(intent)
        }

        fun stop(context: Context) {
            val intent = Intent(context, LkmdbgOverlayService::class.java).setAction(ACTION_STOP)
            context.startService(intent)
        }
    }
}
