package com.smlc666.lkmdbg.data

import android.content.Context
import android.os.Process as AndroidProcess
import com.smlc666.lkmdbg.data.bridge.SessionBridgeClient
import com.smlc666.lkmdbg.data.bridge.SessionBridgeClientImpl
import com.smlc666.lkmdbg.shared.BridgeCommand
import com.smlc666.lkmdbg.shared.BridgeEventBatchReply
import com.smlc666.lkmdbg.shared.BridgeFreezeThreadsReply
import com.smlc666.lkmdbg.shared.BridgeFreezeThreadsRequest
import com.smlc666.lkmdbg.shared.BridgeHelloReply
import com.smlc666.lkmdbg.shared.BridgeGetStopStateReply
import com.smlc666.lkmdbg.shared.BridgeHwpointListReply
import com.smlc666.lkmdbg.shared.BridgeHwpointMutationReply
import com.smlc666.lkmdbg.shared.BridgeHwpointRequest
import com.smlc666.lkmdbg.shared.BridgeImageListReply
import com.smlc666.lkmdbg.shared.BridgeContinueTargetReply
import com.smlc666.lkmdbg.shared.BridgeContinueTargetRequest
import com.smlc666.lkmdbg.shared.BridgeMemoryReadReply
import com.smlc666.lkmdbg.shared.BridgeMemorySearchReply
import com.smlc666.lkmdbg.shared.BridgeMemoryWriteReply
import com.smlc666.lkmdbg.shared.BridgeOpenSessionReply
import com.smlc666.lkmdbg.shared.BridgeProcessListReply
import com.smlc666.lkmdbg.shared.BridgeSetTargetReply
import com.smlc666.lkmdbg.shared.BridgeSetTargetRequest
import com.smlc666.lkmdbg.shared.BridgeSingleStepReply
import com.smlc666.lkmdbg.shared.BridgeSingleStepRequest
import com.smlc666.lkmdbg.shared.BridgeStatusCode
import com.smlc666.lkmdbg.shared.BridgeStatusSnapshot
import com.smlc666.lkmdbg.shared.BridgeThreadListReply
import com.smlc666.lkmdbg.shared.BridgeThreadRegistersReply
import com.smlc666.lkmdbg.shared.BridgeVmaListReply
import com.smlc666.lkmdbg.shared.BridgeWireCodec
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.util.LinkedHashSet

class PipeAgentClient(
    private val context: Context,
) {
    private val lock = Any()
    private var process: java.lang.Process? = null
    private var input: InputStream? = null
    private var output: OutputStream? = null

    companion object {
        private val commonSuPaths = listOf(
            "/system/bin/su",
            "/system/xbin/su",
            "/sbin/su",
            "/vendor/bin/su",
            "/su/bin/su",
            "/data/adb/ksu/bin/su",
            "/debug_ramdisk/su",
        )
    }

    val agentPathHint: String
        get() = BundledAgentInstaller.installedAgentPath(context)

    fun diagnostics(): RootBridgeDiagnostics =
        RootBridgeDiagnostics(
            uid = AndroidProcess.myUid(),
            agentPath = agentPathHint,
            suCandidates = rootBinaryCandidates(),
        )

    fun asSessionBridgeClient(): SessionBridgeClient = SessionBridgeClientImpl(this)

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

    suspend fun queryImages(): BridgeImageListReply = withContext(Dispatchers.IO) {
        synchronized(lock) {
            ensureProcessLocked()
            BridgeWireCodec.writeFrame(requireOutputLocked(), BridgeCommand.QueryImages)
            val (header, payload) = BridgeWireCodec.readFrame(requireInputLocked())
            if (header.command != BridgeCommand.QueryImages.wireId) {
                return@synchronized BridgeImageListReply(
                    status = BridgeStatusCode.InvalidHeader.wireValue,
                    count = 0u,
                    message = "unexpected query-images reply command=${header.command}",
                    images = emptyList(),
                )
            }
            BridgeWireCodec.decodeQueryImagesReply(payload)
        }
    }

    suspend fun queryVmas(): BridgeVmaListReply = withContext(Dispatchers.IO) {
        synchronized(lock) {
            ensureProcessLocked()
            BridgeWireCodec.writeFrame(requireOutputLocked(), BridgeCommand.QueryVmas)
            val (header, payload) = BridgeWireCodec.readFrame(requireInputLocked())
            if (header.command != BridgeCommand.QueryVmas.wireId) {
                return@synchronized BridgeVmaListReply(
                    status = BridgeStatusCode.InvalidHeader.wireValue,
                    count = 0u,
                    message = "unexpected query-vmas reply command=${header.command}",
                    vmas = emptyList(),
                )
            }
            BridgeWireCodec.decodeQueryVmasReply(payload)
        }
    }

    suspend fun readMemory(
        remoteAddr: ULong,
        length: UInt,
        flags: UInt = 0u,
    ): BridgeMemoryReadReply = withContext(Dispatchers.IO) {
        synchronized(lock) {
            ensureProcessLocked()
            BridgeWireCodec.writeFrame(
                requireOutputLocked(),
                BridgeCommand.ReadMemory,
                BridgeWireCodec.encodeMemoryRequest(remoteAddr, length, flags),
            )
            val (header, payload) = BridgeWireCodec.readFrame(requireInputLocked())
            if (header.command != BridgeCommand.ReadMemory.wireId) {
                return@synchronized BridgeMemoryReadReply(
                    status = BridgeStatusCode.InvalidHeader.wireValue,
                    remoteAddr = remoteAddr,
                    requestedLength = length,
                    flags = flags,
                    bytesDone = 0u,
                    message = "unexpected read-memory reply command=${header.command}",
                    data = ByteArray(0),
                )
            }
            BridgeWireCodec.decodeReadMemoryReply(payload)
        }
    }

    suspend fun searchMemory(
        regionPreset: UInt,
        maxResults: UInt,
        pattern: ByteArray,
    ): BridgeMemorySearchReply = withContext(Dispatchers.IO) {
        synchronized(lock) {
            ensureProcessLocked()
            BridgeWireCodec.writeFrame(
                requireOutputLocked(),
                BridgeCommand.SearchMemory,
                BridgeWireCodec.encodeSearchMemoryRequest(regionPreset, maxResults, pattern),
            )
            val (header, payload) = BridgeWireCodec.readFrame(requireInputLocked())
            if (header.command != BridgeCommand.SearchMemory.wireId) {
                return@synchronized BridgeMemorySearchReply(
                    status = BridgeStatusCode.InvalidHeader.wireValue,
                    count = 0u,
                    searchedVmaCount = 0u,
                    scannedBytes = 0u,
                    message = "unexpected search-memory reply command=${header.command}",
                    results = emptyList(),
                )
            }
            BridgeWireCodec.decodeSearchMemoryReply(payload)
        }
    }

    suspend fun getStopState(): BridgeGetStopStateReply = withContext(Dispatchers.IO) {
        synchronized(lock) {
            ensureProcessLocked()
            BridgeWireCodec.writeFrame(requireOutputLocked(), BridgeCommand.GetStopState)
            val (header, payload) = BridgeWireCodec.readFrame(requireInputLocked())
            if (header.command != BridgeCommand.GetStopState.wireId) {
                return@synchronized BridgeGetStopStateReply(
                    status = BridgeStatusCode.InvalidHeader.wireValue,
                    stop = com.smlc666.lkmdbg.shared.BridgeStopState(
                        cookie = 0uL,
                        reason = 0u,
                        flags = 0u,
                        tgid = 0,
                        tid = 0,
                        eventFlags = 0u,
                        value0 = 0uL,
                        value1 = 0uL,
                        x0 = 0uL,
                        x1 = 0uL,
                        x29 = 0uL,
                        x30 = 0uL,
                        sp = 0uL,
                        pc = 0uL,
                        pstate = 0uL,
                        features = 0u,
                        fpsr = 0u,
                        fpcr = 0u,
                    ),
                    message = "unexpected get-stop-state reply command=${header.command}",
                )
            }
            BridgeWireCodec.decodeGetStopStateReply(payload)
        }
    }

    suspend fun freezeThreads(
        request: BridgeFreezeThreadsRequest = BridgeFreezeThreadsRequest(),
    ): BridgeFreezeThreadsReply = withContext(Dispatchers.IO) {
        synchronized(lock) {
            ensureProcessLocked()
            BridgeWireCodec.writeFrame(
                requireOutputLocked(),
                BridgeCommand.FreezeThreads,
                BridgeWireCodec.encodeFreezeThreadsRequest(request),
            )
            val (header, payload) = BridgeWireCodec.readFrame(requireInputLocked())
            if (header.command != BridgeCommand.FreezeThreads.wireId) {
                return@synchronized BridgeFreezeThreadsReply(
                    status = BridgeStatusCode.InvalidHeader.wireValue,
                    flags = request.flags,
                    timeoutMs = request.timeoutMs,
                    threadsTotal = 0u,
                    threadsSettled = 0u,
                    threadsParked = 0u,
                    message = "unexpected freeze-threads reply command=${header.command}",
                )
            }
            BridgeWireCodec.decodeFreezeThreadsReply(payload)
        }
    }

    suspend fun thawThreads(
        request: BridgeFreezeThreadsRequest = BridgeFreezeThreadsRequest(),
    ): BridgeFreezeThreadsReply = withContext(Dispatchers.IO) {
        synchronized(lock) {
            ensureProcessLocked()
            BridgeWireCodec.writeFrame(
                requireOutputLocked(),
                BridgeCommand.ThawThreads,
                BridgeWireCodec.encodeFreezeThreadsRequest(request),
            )
            val (header, payload) = BridgeWireCodec.readFrame(requireInputLocked())
            if (header.command != BridgeCommand.ThawThreads.wireId) {
                return@synchronized BridgeFreezeThreadsReply(
                    status = BridgeStatusCode.InvalidHeader.wireValue,
                    flags = request.flags,
                    timeoutMs = request.timeoutMs,
                    threadsTotal = 0u,
                    threadsSettled = 0u,
                    threadsParked = 0u,
                    message = "unexpected thaw-threads reply command=${header.command}",
                )
            }
            BridgeWireCodec.decodeFreezeThreadsReply(payload)
        }
    }

    suspend fun continueTarget(
        request: BridgeContinueTargetRequest = BridgeContinueTargetRequest(),
    ): BridgeContinueTargetReply = withContext(Dispatchers.IO) {
        synchronized(lock) {
            ensureProcessLocked()
            BridgeWireCodec.writeFrame(
                requireOutputLocked(),
                BridgeCommand.ContinueTarget,
                BridgeWireCodec.encodeContinueTargetRequest(request),
            )
            val (header, payload) = BridgeWireCodec.readFrame(requireInputLocked())
            if (header.command != BridgeCommand.ContinueTarget.wireId) {
                return@synchronized BridgeContinueTargetReply(
                    status = BridgeStatusCode.InvalidHeader.wireValue,
                    flags = request.flags,
                    timeoutMs = request.timeoutMs,
                    stopCookie = request.stopCookie,
                    threadsTotal = 0u,
                    threadsSettled = 0u,
                    threadsParked = 0u,
                    message = "unexpected continue-target reply command=${header.command}",
                )
            }
            BridgeWireCodec.decodeContinueTargetReply(payload)
        }
    }

    suspend fun writeMemory(
        remoteAddr: ULong,
        data: ByteArray,
        flags: UInt = 0u,
    ): BridgeMemoryWriteReply = withContext(Dispatchers.IO) {
        synchronized(lock) {
            ensureProcessLocked()
            BridgeWireCodec.writeFrame(
                requireOutputLocked(),
                BridgeCommand.WriteMemory,
                BridgeWireCodec.encodeMemoryWriteRequest(remoteAddr, data, flags),
            )
            val (header, payload) = BridgeWireCodec.readFrame(requireInputLocked())
            if (header.command != BridgeCommand.WriteMemory.wireId) {
                return@synchronized BridgeMemoryWriteReply(
                    status = BridgeStatusCode.InvalidHeader.wireValue,
                    remoteAddr = remoteAddr,
                    requestedLength = data.size.toUInt(),
                    flags = flags,
                    bytesDone = 0u,
                    message = "unexpected write-memory reply command=${header.command}",
                )
            }
            BridgeWireCodec.decodeWriteMemoryReply(payload)
        }
    }

    suspend fun addHwpoint(
        request: BridgeHwpointRequest,
    ): BridgeHwpointMutationReply = withContext(Dispatchers.IO) {
        synchronized(lock) {
            ensureProcessLocked()
            BridgeWireCodec.writeFrame(
                requireOutputLocked(),
                BridgeCommand.AddHwpoint,
                BridgeWireCodec.encodeHwpointRequest(request),
            )
            val (header, payload) = BridgeWireCodec.readFrame(requireInputLocked())
            if (header.command != BridgeCommand.AddHwpoint.wireId) {
                return@synchronized BridgeHwpointMutationReply(
                    status = BridgeStatusCode.InvalidHeader.wireValue,
                    id = request.id,
                    addr = request.addr,
                    tid = request.tid,
                    type = request.type,
                    len = request.len,
                    flags = request.flags,
                    triggerHitCount = request.triggerHitCount,
                    actionFlags = request.actionFlags,
                    message = "unexpected add-hwpoint reply command=${header.command}",
                )
            }
            BridgeWireCodec.decodeHwpointMutationReply(payload)
        }
    }

    suspend fun removeHwpoint(
        request: BridgeHwpointRequest,
    ): BridgeHwpointMutationReply = withContext(Dispatchers.IO) {
        synchronized(lock) {
            ensureProcessLocked()
            BridgeWireCodec.writeFrame(
                requireOutputLocked(),
                BridgeCommand.RemoveHwpoint,
                BridgeWireCodec.encodeHwpointRequest(request),
            )
            val (header, payload) = BridgeWireCodec.readFrame(requireInputLocked())
            if (header.command != BridgeCommand.RemoveHwpoint.wireId) {
                return@synchronized BridgeHwpointMutationReply(
                    status = BridgeStatusCode.InvalidHeader.wireValue,
                    id = request.id,
                    addr = request.addr,
                    tid = request.tid,
                    type = request.type,
                    len = request.len,
                    flags = request.flags,
                    triggerHitCount = request.triggerHitCount,
                    actionFlags = request.actionFlags,
                    message = "unexpected remove-hwpoint reply command=${header.command}",
                )
            }
            BridgeWireCodec.decodeHwpointMutationReply(payload)
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
                    sp = 0uL,
                    pc = 0uL,
                    pstate = 0uL,
                    features = 0u,
                    fpsr = 0u,
                    fpcr = 0u,
                    v0Lo = 0uL,
                    v0Hi = 0uL,
                    message = "unexpected get-registers reply command=${header.command}",
                )
            }
            BridgeWireCodec.decodeGetRegistersReply(payload)
        }
    }

    suspend fun singleStep(
        request: BridgeSingleStepRequest,
    ): BridgeSingleStepReply = withContext(Dispatchers.IO) {
        synchronized(lock) {
            ensureProcessLocked()
            BridgeWireCodec.writeFrame(
                requireOutputLocked(),
                BridgeCommand.SingleStep,
                BridgeWireCodec.encodeSingleStepRequest(request),
            )
            val (header, payload) = BridgeWireCodec.readFrame(requireInputLocked())
            if (header.command != BridgeCommand.SingleStep.wireId) {
                return@synchronized BridgeSingleStepReply(
                    status = BridgeStatusCode.InvalidHeader.wireValue,
                    tid = request.tid,
                    flags = request.flags,
                    message = "unexpected single-step reply command=${header.command}",
                )
            }
            BridgeWireCodec.decodeSingleStepReply(payload)
        }
    }

    suspend fun queryHwpoints(): BridgeHwpointListReply = withContext(Dispatchers.IO) {
        synchronized(lock) {
            ensureProcessLocked()
            BridgeWireCodec.writeFrame(requireOutputLocked(), BridgeCommand.QueryHwpoints)
            val (header, payload) = BridgeWireCodec.readFrame(requireInputLocked())
            if (header.command != BridgeCommand.QueryHwpoints.wireId) {
                return@synchronized BridgeHwpointListReply(
                    status = BridgeStatusCode.InvalidHeader.wireValue,
                    count = 0u,
                    message = "unexpected query-hwpoints reply command=${header.command}",
                    hwpoints = emptyList(),
                )
            }
            BridgeWireCodec.decodeQueryHwpointsReply(payload)
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
        val started = startRootBridge(agentPath)
        process = started
        input = started.inputStream
        output = started.outputStream
    }

    private fun startRootBridge(agentPath: String): Process {
        val attempts = ArrayList<String>()
        var lastError: IOException? = null
        val directArgv = listOf(agentPath, "--stdio")

        if (AndroidProcess.myUid() == 0) {
            attempts += directArgv.joinToString(" ")
            try {
                return ProcessBuilder(directArgv).start()
            } catch (e: IOException) {
                lastError = e
            }
        }

        val command = "$agentPath --stdio"

        rootBinaryCandidates().forEach { suBinary ->
            val argv = listOf(suBinary, "-c", command)
            attempts += argv.joinToString(" ")
            try {
                return ProcessBuilder(argv).start()
            } catch (e: IOException) {
                lastError = e
            }
        }

        val suffix = lastError?.message?.takeIf { it.isNotBlank() } ?: "no working su binary"
        throw IOException(
            "unable to start root bridge via su; tried ${attempts.joinToString(" | ")}; last error: $suffix",
            lastError,
        )
    }

    private fun rootBinaryCandidates(): List<String> {
        val candidates = LinkedHashSet<String>()
        candidates += "su"

        val pathValue = System.getenv("PATH").orEmpty()
        pathValue.split(':')
            .filter { it.isNotBlank() }
            .forEach { directory ->
                candidates += "$directory/su"
            }

        commonSuPaths.forEach { candidates += it }

        return candidates.filter { candidate ->
            if (!candidate.contains('/'))
                true
            else
                File(candidate).canExecute()
        }
    }

    private fun requireInputLocked(): InputStream =
        input ?: error("agent input stream unavailable")

    private fun requireOutputLocked(): OutputStream =
        output ?: error("agent output stream unavailable")
}
