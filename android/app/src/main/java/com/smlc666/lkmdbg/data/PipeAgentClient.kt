package com.smlc666.lkmdbg.data

import android.content.Context
import com.smlc666.lkmdbg.shared.BridgeCommand
import com.smlc666.lkmdbg.shared.BridgeEventBatchReply
import com.smlc666.lkmdbg.shared.BridgeHelloReply
import com.smlc666.lkmdbg.shared.BridgeOpenSessionReply
import com.smlc666.lkmdbg.shared.BridgeProcessListReply
import com.smlc666.lkmdbg.shared.BridgeSetTargetReply
import com.smlc666.lkmdbg.shared.BridgeSetTargetRequest
import com.smlc666.lkmdbg.shared.BridgeStatusCode
import com.smlc666.lkmdbg.shared.BridgeStatusSnapshot
import com.smlc666.lkmdbg.shared.BridgeThreadListReply
import com.smlc666.lkmdbg.shared.BridgeThreadRegistersReply
import com.smlc666.lkmdbg.shared.BridgeWireCodec
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.InputStream
import java.io.OutputStream

class PipeAgentClient(
    private val context: Context,
) {
    private val lock = Any()
    private var process: Process? = null
    private var input: InputStream? = null
    private var output: OutputStream? = null

    val agentPathHint: String
        get() = BundledAgentInstaller.installedAgentPath(context)

    suspend fun connect(): BridgeHelloReply = withContext(Dispatchers.IO) {
        synchronized(lock) {
            ensureProcessLocked()
            BridgeWireCodec.writeFrame(requireOutputLocked(), BridgeCommand.Hello)
            val (header, payload) = BridgeWireCodec.readFrame(requireInputLocked())
            if (header.command != BridgeCommand.Hello.wireId) {
                return@synchronized BridgeHelloReply(
                    status = BridgeStatusCode.InvalidHeader.wireValue,
                    serverVersion = 0u,
                    featureBits = 0u,
                    message = "unexpected hello reply command=${header.command}",
                )
            }
            BridgeWireCodec.decodeHelloReply(payload)
        }
    }

    suspend fun openSession(): BridgeOpenSessionReply = withContext(Dispatchers.IO) {
        synchronized(lock) {
            ensureProcessLocked()
            BridgeWireCodec.writeFrame(requireOutputLocked(), BridgeCommand.OpenSession)
            val (header, payload) = BridgeWireCodec.readFrame(requireInputLocked())
            if (header.command != BridgeCommand.OpenSession.wireId) {
                return@synchronized BridgeOpenSessionReply(
                    status = BridgeStatusCode.InvalidHeader.wireValue,
                    sessionOpen = false,
                    sessionId = 0u,
                    message = "unexpected open-session reply command=${header.command}",
                )
            }
            BridgeWireCodec.decodeOpenSessionReply(payload)
        }
    }

    suspend fun setTarget(targetPid: Int, targetTid: Int = 0): BridgeSetTargetReply =
        withContext(Dispatchers.IO) {
            synchronized(lock) {
                ensureProcessLocked()
                BridgeWireCodec.writeFrame(
                    requireOutputLocked(),
                    BridgeCommand.SetTarget,
                    BridgeWireCodec.encodeSetTargetRequest(
                        BridgeSetTargetRequest(
                            targetPid = targetPid,
                            targetTid = targetTid,
                        ),
                    ),
                )
                val (header, payload) = BridgeWireCodec.readFrame(requireInputLocked())
                if (header.command != BridgeCommand.SetTarget.wireId) {
                    return@synchronized BridgeSetTargetReply(
                        status = BridgeStatusCode.InvalidHeader.wireValue,
                        targetPid = targetPid,
                        targetTid = targetTid,
                        message = "unexpected set-target reply command=${header.command}",
                    )
                }
                BridgeWireCodec.decodeSetTargetReply(payload)
            }
        }

    suspend fun statusSnapshot(): BridgeStatusSnapshot = withContext(Dispatchers.IO) {
        synchronized(lock) {
            ensureProcessLocked()
            BridgeWireCodec.writeFrame(requireOutputLocked(), BridgeCommand.StatusSnapshot)
            val (header, payload) = BridgeWireCodec.readFrame(requireInputLocked())
            if (header.command != BridgeCommand.StatusSnapshot.wireId) {
                return@synchronized BridgeStatusSnapshot(
                    status = BridgeStatusCode.InvalidHeader.wireValue,
                    connected = false,
                    targetPid = 0,
                    targetTid = 0,
                    sessionOpen = false,
                    agentPid = 0,
                    ownerPid = 0,
                    hookActive = 0,
                    eventQueueDepth = 0u,
                    sessionId = 0u,
                    transport = "stdio-pipe",
                    message = "unexpected status reply command=${header.command}",
                )
            }
            BridgeWireCodec.decodeStatusSnapshot(payload)
        }
    }

    suspend fun queryProcesses(): BridgeProcessListReply = withContext(Dispatchers.IO) {
        synchronized(lock) {
            ensureProcessLocked()
            BridgeWireCodec.writeFrame(requireOutputLocked(), BridgeCommand.QueryProcesses)
            val (header, payload) = BridgeWireCodec.readFrame(requireInputLocked())
            if (header.command != BridgeCommand.QueryProcesses.wireId) {
                return@synchronized BridgeProcessListReply(
                    status = BridgeStatusCode.InvalidHeader.wireValue,
                    count = 0u,
                    message = "unexpected query-processes reply command=${header.command}",
                    processes = emptyList(),
                )
            }
            BridgeWireCodec.decodeQueryProcessesReply(payload)
        }
    }

    suspend fun queryThreads(): BridgeThreadListReply = withContext(Dispatchers.IO) {
        synchronized(lock) {
            ensureProcessLocked()
            BridgeWireCodec.writeFrame(requireOutputLocked(), BridgeCommand.QueryThreads)
            val (header, payload) = BridgeWireCodec.readFrame(requireInputLocked())
            if (header.command != BridgeCommand.QueryThreads.wireId) {
                return@synchronized BridgeThreadListReply(
                    status = BridgeStatusCode.InvalidHeader.wireValue,
                    count = 0u,
                    done = true,
                    nextTid = 0,
                    message = "unexpected query-threads reply command=${header.command}",
                    threads = emptyList(),
                )
            }
            BridgeWireCodec.decodeQueryThreadsReply(payload)
        }
    }

    suspend fun getRegisters(tid: Int): BridgeThreadRegistersReply = withContext(Dispatchers.IO) {
        synchronized(lock) {
            ensureProcessLocked()
            BridgeWireCodec.writeFrame(
                requireOutputLocked(),
                BridgeCommand.GetRegisters,
                BridgeWireCodec.encodeThreadRequest(tid),
            )
            val (header, payload) = BridgeWireCodec.readFrame(requireInputLocked())
            if (header.command != BridgeCommand.GetRegisters.wireId) {
                return@synchronized BridgeThreadRegistersReply(
                    status = BridgeStatusCode.InvalidHeader.wireValue,
                    tid = tid,
                    flags = 0u,
                    regs = ULongArray(31),
                    sp = 0u,
                    pc = 0u,
                    pstate = 0u,
                    features = 0u,
                    fpsr = 0u,
                    fpcr = 0u,
                    v0Lo = 0u,
                    v0Hi = 0u,
                    message = "unexpected get-registers reply command=${header.command}",
                )
            }
            BridgeWireCodec.decodeGetRegistersReply(payload)
        }
    }

    suspend fun pollEvents(timeoutMs: Int, maxEvents: Int): BridgeEventBatchReply =
        withContext(Dispatchers.IO) {
            synchronized(lock) {
                ensureProcessLocked()
                BridgeWireCodec.writeFrame(
                    requireOutputLocked(),
                    BridgeCommand.PollEvent,
                    BridgeWireCodec.encodePollEventsRequest(timeoutMs, maxEvents),
                )
                val (header, payload) = BridgeWireCodec.readFrame(requireInputLocked())
                if (header.command != BridgeCommand.PollEvent.wireId) {
                    return@synchronized BridgeEventBatchReply(
                        status = BridgeStatusCode.InvalidHeader.wireValue,
                        count = 0u,
                        message = "unexpected poll-events reply command=${header.command}",
                        events = emptyList(),
                    )
                }
                BridgeWireCodec.decodePollEventsReply(payload)
            }
        }

    suspend fun close() = withContext(Dispatchers.IO) {
        synchronized(lock) {
            process?.destroy()
            process = null
            input = null
            output = null
        }
    }

    private fun ensureProcessLocked() {
        val current = process
        if (current != null && current.isAlive)
            return

        val agentPath = BundledAgentInstaller.install(context)
        val started = ProcessBuilder("su", "-c", "$agentPath --stdio").start()
        process = started
        input = started.inputStream
        output = started.outputStream
    }

    private fun requireInputLocked(): InputStream =
        input ?: error("agent input stream unavailable")

    private fun requireOutputLocked(): OutputStream =
        output ?: error("agent output stream unavailable")
}
