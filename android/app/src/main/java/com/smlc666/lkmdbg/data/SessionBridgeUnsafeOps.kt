package com.smlc666.lkmdbg.data

import android.content.Context
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.shared.BridgeEventBatchReply
import com.smlc666.lkmdbg.shared.BridgeHelloReply
import com.smlc666.lkmdbg.shared.BridgeImageListReply
import com.smlc666.lkmdbg.shared.BridgeOpenSessionReply
import com.smlc666.lkmdbg.shared.BridgeProcessListReply
import com.smlc666.lkmdbg.shared.BridgeStatusSnapshot
import com.smlc666.lkmdbg.shared.BridgeThreadListReply
import com.smlc666.lkmdbg.shared.BridgeThreadRecord
import com.smlc666.lkmdbg.shared.BridgeVmaListReply
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.update

internal class SessionBridgeUnsafeOps(
    context: Context,
    private val client: PipeAgentClient,
    private val stateFlow: MutableStateFlow<SessionBridgeState>,
    private val processResolver: AndroidProcessResolver,
    private val memoryPreviewBuilder: MemoryPreviewBuilder,
    private val discardMemorySearchSnapshot: () -> Unit,
) {
    private val appContext = context.applicationContext

    suspend fun connect(): BridgeHelloReply {
        val reply = client.connect()
        ensureBridgeStatusOk(reply.status, reply.message, "HELLO")
        stateFlow.update { current ->
            current.copy(
                hello = reply,
                lastMessage = reply.message.ifBlank {
                    appContext.getString(R.string.session_message_connected)
                },
            )
        }
        return reply
    }

    suspend fun openSession(): BridgeOpenSessionReply {
        val reply = client.openSession()
        ensureBridgeStatusOk(reply.status, reply.message, "OPEN_SESSION")
        if (!reply.sessionOpen)
            throw IllegalStateException(reply.message.ifBlank { "OPEN_SESSION did not open a session" })
        stateFlow.update { current ->
            current.copy(
                lastMessage = reply.message.ifBlank {
                    appContext.getString(R.string.session_message_opened)
                },
            )
        }
        return reply
    }

    suspend fun refreshStatus(): BridgeStatusSnapshot {
        val snapshot = client.statusSnapshot()
        ensureBridgeStatusOk(snapshot.status, snapshot.message, "STATUS_SNAPSHOT")
        stateFlow.update { current ->
            current.copy(
                snapshot = snapshot,
                lastMessage = snapshot.message.ifBlank {
                    appContext.getString(R.string.session_message_refreshed)
                },
            )
        }
        return snapshot
    }

    suspend fun attachTarget(targetPid: Int, targetTid: Int = 0) {
        discardMemorySearchSnapshot()
        val reply = client.setTarget(targetPid, targetTid)
        ensureBridgeStatusOk(reply.status, reply.message, "SET_TARGET")
        stateFlow.update { current ->
            current.copy(
                selectedProcessPid = targetPid,
                targetPidInput = targetPid.toString(),
                targetTidInput = targetTid.takeIf { it > 0 }?.toString().orEmpty(),
                memorySearch = current.memorySearch.copy(
                    snapshotReady = false,
                    summary = "",
                    results = emptyList(),
                ),
                lastMessage = reply.message.ifBlank {
                    appContext.getString(R.string.session_message_target_attached, targetPid)
                },
            )
        }
        refreshStatus()
    }

    suspend fun refreshThreads(preferredTid: Int? = null, updateMessage: Boolean = true) {
        val reply: BridgeThreadListReply = client.queryThreads()
        ensureBridgeStatusOk(reply.status, reply.message, "QUERY_THREADS")
        val selectedTid = selectThreadId(reply.threads, preferredTid)
        val registers = selectedTid?.let {
            runCatching {
                client.getRegisters(it).also { registerReply ->
                    ensureBridgeStatusOk(registerReply.status, registerReply.message, "GET_REGISTERS")
                }
            }.getOrNull()
        }
        stateFlow.update { current ->
            current.copy(
                threads = reply.threads,
                selectedThreadTid = selectedTid,
                selectedThreadRegisters = registers,
                lastMessage = if (updateMessage) {
                    reply.message.ifBlank {
                        appContext.getString(R.string.thread_message_refreshed, reply.threads.size)
                    }
                } else {
                    current.lastMessage
                },
            )
        }
    }

    suspend fun refreshEvents(timeoutMs: Int, maxEvents: Int) {
        val reply: BridgeEventBatchReply = client.pollEvents(timeoutMs, maxEvents)
        ensureBridgeStatusOk(reply.status, reply.message, "POLL_EVENT")
        val now = System.currentTimeMillis()
        stateFlow.update { current ->
            val merged = buildList {
                reply.events.forEach { add(SessionEventEntry(it, now)) }
                current.recentEvents.forEach { existing ->
                    if (none { it.record.seq == existing.record.seq })
                        add(existing)
                }
            }.take(256)
            val survivingPins = current.pinnedEventSeqs.filterTo(linkedSetOf()) { pinnedSeq ->
                merged.any { it.record.seq == pinnedSeq }
            }
            current.copy(
                recentEvents = merged,
                pinnedEventSeqs = survivingPins,
                lastMessage = reply.message.ifBlank {
                    appContext.getString(R.string.event_message_refreshed, reply.events.size)
                },
            )
        }
    }

    suspend fun refreshImages() {
        val reply: BridgeImageListReply = client.queryImages()
        ensureBridgeStatusOk(reply.status, reply.message, "QUERY_IMAGES")
        stateFlow.update { current ->
            current.copy(
                images = reply.images,
                lastMessage = reply.message.ifBlank {
                    appContext.getString(R.string.memory_message_images, reply.images.size)
                },
            )
        }
    }

    suspend fun refreshVmas() {
        val reply: BridgeVmaListReply = client.queryVmas()
        ensureBridgeStatusOk(reply.status, reply.message, "QUERY_VMAS")
        stateFlow.update { current ->
            current.copy(
                vmas = reply.vmas,
                memoryPage = current.memoryPage?.let { page ->
                    memoryPreviewBuilder.buildPage(page.focusAddress, page.pageStart, page.bytes, reply.vmas)
                },
                lastMessage = reply.message.ifBlank {
                    appContext.getString(R.string.memory_message_vmas, reply.vmas.size)
                },
            )
        }
    }

    suspend fun refreshProcesses() {
        val reply: BridgeProcessListReply = client.queryProcesses()
        ensureBridgeStatusOk(reply.status, reply.message, "QUERY_PROCESSES")
        val resolved = processResolver.resolve(reply.processes)
        stateFlow.update { current ->
            current.copy(
                processes = resolved,
                lastMessage = reply.message.ifBlank {
                    appContext.getString(R.string.process_message_refreshed, resolved.size)
                },
            )
        }
    }

    private fun selectThreadId(
        threads: List<BridgeThreadRecord>,
        preferredTid: Int?,
    ): Int? {
        if (threads.isEmpty())
            return null
        val current = preferredTid
            ?: stateFlow.value.selectedThreadTid
            ?: stateFlow.value.snapshot.targetTid.takeIf { it > 0 }
        if (current != null && threads.any { it.tid == current })
            return current
        return threads.firstOrNull()?.tid
    }
}
