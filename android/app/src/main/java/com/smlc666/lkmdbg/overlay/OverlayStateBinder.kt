package com.smlc666.lkmdbg.overlay

import android.content.Context
import com.smlc666.lkmdbg.data.SessionBridgeRepository
import com.smlc666.lkmdbg.nativeui.NativeWorkspaceTextureView
import com.smlc666.lkmdbg.nativeui.toNativeWorkspaceSnapshot
import com.smlc666.lkmdbg.shell.BridgeStatusFormatter
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.launch

internal class OverlayStateBinder(
    private val context: Context,
    private val repository: SessionBridgeRepository,
    private val headerController: OverlayHeaderController,
    private val processPickerController: OverlayProcessPickerController,
) {
    fun bind(
        scope: CoroutineScope,
        expanded: Boolean,
        workspaceView: () -> NativeWorkspaceTextureView?,
    ): Job =
        scope.launch {
            repository.state.collect { state ->
                workspaceView()?.updateSnapshot(
                    state.toNativeWorkspaceSnapshot(context, expanded),
                )
                headerController.renderStatus(
                    BridgeStatusFormatter.formatOverlayStatus(context, state),
                )
                processPickerController.render(state)
            }
        }
}
