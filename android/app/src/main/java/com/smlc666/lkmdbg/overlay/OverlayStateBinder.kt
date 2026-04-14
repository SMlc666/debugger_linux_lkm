package com.smlc666.lkmdbg.overlay

import com.smlc666.lkmdbg.data.SessionBridgeRepository
import com.smlc666.lkmdbg.data.SessionBridgeState
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.launch

internal class OverlayStateBinder(
    private val repository: SessionBridgeRepository,
    private val onStateChanged: (prev: SessionBridgeState?, next: SessionBridgeState) -> Unit,
) {
    fun bind(scope: CoroutineScope): Job =
        scope.launch {
            var prev: SessionBridgeState? = null
            repository.state.collect { state ->
                onStateChanged(prev, state)
                prev = state
            }
        }
}
