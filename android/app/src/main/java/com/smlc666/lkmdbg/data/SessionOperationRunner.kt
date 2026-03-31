package com.smlc666.lkmdbg.data

import android.content.Context
import com.smlc666.lkmdbg.R
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.update
import java.io.IOException

internal class SessionOperationRunner(
    context: Context,
    private val stateFlow: MutableStateFlow<SessionBridgeState>,
) {
    private val appContext = context.applicationContext

    fun updateMessage(message: String) {
        stateFlow.update { current -> current.copy(lastMessage = message) }
    }

    suspend fun <T> run(block: suspend () -> T): T? {
        stateFlow.update { current -> current.copy(busy = true) }
        try {
            return block()
        } catch (e: IOException) {
            updateError(
                appContext.getString(
                    R.string.session_error_io,
                    e.message ?: appContext.getString(R.string.session_error_unknown),
                ),
            )
        } catch (e: IllegalStateException) {
            updateError(
                appContext.getString(
                    R.string.session_error_bridge,
                    e.message ?: appContext.getString(R.string.session_error_unknown),
                ),
            )
        } catch (e: RuntimeException) {
            updateError(
                appContext.getString(
                    R.string.session_error_bridge,
                    e.message ?: appContext.getString(R.string.session_error_unknown),
                ),
            )
        } finally {
            stateFlow.update { current -> current.copy(busy = false) }
        }
        return null
    }

    private fun updateError(message: String) {
        stateFlow.update { current -> current.copy(lastMessage = message) }
    }
}
