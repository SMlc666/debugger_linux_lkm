package com.smlc666.lkmdbg.shell

import com.smlc666.lkmdbg.data.SessionBridgeRepository
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

class SessionAutomationController(
    private val repository: SessionBridgeRepository,
) {
    private val actionMutex = Mutex()
    private var warmStartJob: Job? = null
    private var statusLoopJob: Job? = null

    fun requestWarmStart(scope: CoroutineScope): Job {
        warmStartJob?.takeIf { it.isActive }?.let { return it }
        val job = scope.launch(Dispatchers.Main.immediate) {
            actionMutex.withLock {
                val snapshot = repository.state.first()
                if (snapshot.busy)
                    return@withLock
                if (snapshot.hello == null)
                    repository.connect()
                if (!repository.state.value.snapshot.sessionOpen)
                    repository.openSession()
                else
                    repository.refreshStatus()
                if (repository.state.value.processes.isEmpty())
                    repository.refreshProcesses()
            }
        }
        warmStartJob = job
        return job
    }

    fun startStatusLoop(
        scope: CoroutineScope,
        intervalMs: Long = 2000L,
        processRefreshIntervalMs: Long = 10000L,
    ) {
        if (statusLoopJob?.isActive == true)
            return
        statusLoopJob = scope.launch(Dispatchers.Main.immediate) {
            var lastProcessRefreshAt = 0L
            while (isActive) {
                actionMutex.withLock {
                    val state = repository.state.value
                    if (!state.busy && state.hello != null) {
                        repository.refreshStatus()
                        if (state.snapshot.sessionOpen &&
                            (state.processes.isEmpty() ||
                                System.currentTimeMillis() - lastProcessRefreshAt >= processRefreshIntervalMs)
                        ) {
                            repository.refreshProcesses()
                            lastProcessRefreshAt = System.currentTimeMillis()
                        }
                    }
                }
                delay(intervalMs)
            }
        }
    }

    fun stopStatusLoop() {
        statusLoopJob?.cancel()
        statusLoopJob = null
    }
}
