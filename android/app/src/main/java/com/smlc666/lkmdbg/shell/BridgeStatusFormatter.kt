package com.smlc666.lkmdbg.shell

import android.content.Context
import com.smlc666.lkmdbg.appdata.R
import com.smlc666.lkmdbg.data.SessionBridgeState

internal object BridgeStatusFormatter {
    fun formatCompact(context: Context, state: SessionBridgeState): String =
        buildString {
            append("transport=")
            append(state.snapshot.transport)
            append('\n')
            append("pid=")
            append(state.snapshot.targetPid)
            append(" tid=")
            append(state.snapshot.targetTid)
            append(" sid=0x")
            append(state.snapshot.sessionId.toString(16))
            append('\n')
            append("proc=")
            append(state.processes.size)
            append(" thr=")
            append(state.threads.size)
            append(" evt=")
            append(state.recentEvents.size)
            append(" hook=")
            append(state.snapshot.hookActive)
            append('\n')
            append(state.lastMessage)
        }

    fun formatOverlayStatus(context: Context, state: SessionBridgeState): String =
        context.getString(
            R.string.overlay_status_template,
            state.snapshot.transport,
            state.snapshot.targetPid,
            state.snapshot.targetTid,
            "0x${state.snapshot.sessionId.toString(16)}",
        )
}
