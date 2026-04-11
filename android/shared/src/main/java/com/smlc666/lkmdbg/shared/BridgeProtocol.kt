package com.smlc666.lkmdbg.shared

import java.io.EOFException
import java.io.InputStream
import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.StandardCharsets

object BridgeProtocol {
    const val VERSION: UInt = 1u
    const val MAGIC: UInt = 0x4C4B4D44u
    const val HEADER_SIZE: Int = 16
    const val HELLO_REPLY_SIZE: Int = 80
    const val OPEN_SESSION_REPLY_SIZE: Int = 80
    const val SET_TARGET_REQUEST_SIZE: Int = 8
    const val SET_TARGET_REPLY_SIZE: Int = 72
    const val STATUS_SNAPSHOT_REPLY_SIZE: Int = 140
    const val QUERY_PROCESSES_REPLY_HEADER_SIZE: Int = 72
    const val QUERY_PROCESS_RECORD_SIZE: Int = 200
    const val READ_MEMORY_REQUEST_SIZE: Int = 16
    const val READ_MEMORY_REPLY_HEADER_SIZE: Int = 88
    const val WRITE_MEMORY_REQUEST_HEADER_SIZE: Int = 16
    const val WRITE_MEMORY_REPLY_SIZE: Int = 88
    const val QUERY_THREADS_REPLY_HEADER_SIZE: Int = 80
    const val QUERY_THREAD_RECORD_SIZE: Int = 48
    const val GET_REGISTERS_REQUEST_SIZE: Int = 8
    const val GET_REGISTERS_REPLY_SIZE: Int = 384
    const val POLL_EVENTS_REQUEST_SIZE: Int = 8
    const val POLL_EVENTS_REPLY_HEADER_SIZE: Int = 72
    const val POLL_EVENT_RECORD_SIZE: Int = 64
    const val QUERY_IMAGES_REPLY_HEADER_SIZE: Int = 72
    const val QUERY_IMAGE_RECORD_SIZE: Int = 320
    const val QUERY_VMAS_REPLY_HEADER_SIZE: Int = 72
    const val QUERY_VMA_RECORD_SIZE: Int = 320
    const val SEARCH_MEMORY_REQUEST_SIZE: Int = 144
    const val SEARCH_MEMORY_REPLY_HEADER_SIZE: Int = 88
    const val SEARCH_MEMORY_RESULT_RECORD_SIZE: Int = 64
    const val FREEZE_THREADS_REQUEST_SIZE: Int = 8
    const val FREEZE_THREADS_REPLY_SIZE: Int = 88
    const val CONTINUE_TARGET_REQUEST_SIZE: Int = 16
    const val CONTINUE_TARGET_REPLY_SIZE: Int = 96
    const val SINGLE_STEP_REQUEST_SIZE: Int = 8
    const val SINGLE_STEP_REPLY_SIZE: Int = 76
    const val GET_STOP_STATE_REPLY_SIZE: Int = 180
    const val HWPOINT_REQUEST_SIZE: Int = 48
    const val HWPOINT_MUTATION_REPLY_SIZE: Int = 112
    const val QUERY_HWPOINTS_REPLY_HEADER_SIZE: Int = 72
    const val QUERY_HWPOINT_RECORD_SIZE: Int = 64
}

enum class BridgeCommand(val wireId: UInt) {
    Hello(1u),
    OpenSession(2u),
    SetTarget(3u),
    ReadMemory(4u),
    WriteMemory(5u),
    QueryThreads(6u),
    GetRegisters(7u),
    EventStreamConfig(8u),
    PollEvent(9u),
    StatusSnapshot(10u),
    QueryProcesses(11u),
    QueryImages(12u),
    QueryVmas(13u),
    SearchMemory(14u),
    GetStopState(15u),
    FreezeThreads(16u),
    ThawThreads(17u),
    ContinueTarget(18u),
    SingleStep(19u),
    QueryHwpoints(20u),
    AddHwpoint(21u),
    RemoveHwpoint(22u),
}

enum class BridgeStatusCode(val wireValue: Int) {
    Ok(0),
    Unsupported(-1),
    InvalidHeader(-2),
    InvalidPayload(-3),
    InternalError(-4),
    NoSession(-5),
}

data class BridgeFrameHeader(
    val magic: UInt = BridgeProtocol.MAGIC,
    val version: UInt = BridgeProtocol.VERSION,
    val command: UInt,
    val payloadSize: UInt,
)

data class BridgeHelloReply(
    val status: Int,
    val serverVersion: UInt,
    val featureBits: ULong,
    val message: String,
)

data class BridgeOpenSessionReply(
    val status: Int,
    val sessionOpen: Boolean,
    val sessionId: ULong,
    val message: String,
)

data class BridgeSetTargetRequest(
    val targetPid: Int,
    val targetTid: Int = 0,
)

data class BridgeSetTargetReply(
    val status: Int,
    val targetPid: Int,
    val targetTid: Int,
    val message: String,
)

data class BridgeStatusSnapshot(
    val status: Int,
    val connected: Boolean,
    val targetPid: Int,
    val targetTid: Int,
    val sessionOpen: Boolean,
    val agentPid: Int,
    val ownerPid: Int,
    val hookActive: Int,
    val eventQueueDepth: UInt,
    val sessionId: ULong,
    val transport: String,
    val message: String,
)

data class BridgeProcessRecord(
    val pid: Int,
    val uid: Int,
    val comm: String,
    val cmdline: String,
)

data class BridgeProcessListReply(
    val status: Int,
    val count: UInt,
    val message: String,
    val processes: List<BridgeProcessRecord>,
)

data class BridgeImageRecord(
    val startAddr: ULong,
    val endAddr: ULong,
    val baseAddr: ULong,
    val pgoff: ULong,
    val inode: ULong,
    val prot: UInt,
    val flags: UInt,
    val devMajor: UInt,
    val devMinor: UInt,
    val segmentCount: UInt,
    val name: String,
)

data class BridgeImageListReply(
    val status: Int,
    val count: UInt,
    val message: String,
    val images: List<BridgeImageRecord>,
)

data class BridgeVmaRecord(
    val startAddr: ULong,
    val endAddr: ULong,
    val pgoff: ULong,
    val inode: ULong,
    val vmFlagsRaw: ULong,
    val prot: UInt,
    val flags: UInt,
    val devMajor: UInt,
    val devMinor: UInt,
    val name: String,
)

data class BridgeVmaListReply(
    val status: Int,
    val count: UInt,
    val message: String,
    val vmas: List<BridgeVmaRecord>,
)

data class BridgeMemorySearchRecord(
    val address: ULong,
    val regionStart: ULong,
    val regionEnd: ULong,
    val previewSize: UInt,
    val preview: ByteArray,
)

data class BridgeMemorySearchReply(
    val status: Int,
    val count: UInt,
    val searchedVmaCount: UInt,
    val scannedBytes: ULong,
    val message: String,
    val results: List<BridgeMemorySearchRecord>,
)

data class BridgeThreadRecord(
    val tid: Int,
    val tgid: Int,
    val flags: UInt,
    val userPc: ULong,
    val userSp: ULong,
    val comm: String,
)

data class BridgeThreadListReply(
    val status: Int,
    val count: UInt,
    val done: Boolean,
    val nextTid: Int,
    val message: String,
    val threads: List<BridgeThreadRecord>,
)

data class BridgeThreadRegistersReply(
    val status: Int,
    val tid: Int,
    val flags: UInt,
    val regs: ULongArray,
    val sp: ULong,
    val pc: ULong,
    val pstate: ULong,
    val features: UInt,
    val fpsr: UInt,
    val fpcr: UInt,
    val v0Lo: ULong,
    val v0Hi: ULong,
    val message: String,
)

data class BridgeMemoryReadReply(
    val status: Int,
    val remoteAddr: ULong,
    val requestedLength: UInt,
    val flags: UInt,
    val bytesDone: UInt,
    val message: String,
    val data: ByteArray,
)

data class BridgeMemoryWriteReply(
    val status: Int,
    val remoteAddr: ULong,
    val requestedLength: UInt,
    val flags: UInt,
    val bytesDone: UInt,
    val message: String,
)

data class BridgeEventRecord(
    val version: UInt,
    val type: UInt,
    val size: UInt,
    val code: UInt,
    val sessionId: ULong,
    val seq: ULong,
    val tgid: Int,
    val tid: Int,
    val flags: UInt,
    val value0: ULong,
    val value1: ULong,
)

data class BridgeEventBatchReply(
    val status: Int,
    val count: UInt,
    val message: String,
    val events: List<BridgeEventRecord>,
)

data class BridgeFreezeThreadsRequest(
    val timeoutMs: Int = 1000,
    val flags: UInt = 0u,
)

data class BridgeFreezeThreadsReply(
    val status: Int,
    val flags: UInt,
    val timeoutMs: Int,
    val threadsTotal: UInt,
    val threadsSettled: UInt,
    val threadsParked: UInt,
    val message: String,
)

data class BridgeContinueTargetRequest(
    val stopCookie: ULong = 0uL,
    val timeoutMs: Int = 1000,
    val flags: UInt = 0u,
)

data class BridgeContinueTargetReply(
    val status: Int,
    val flags: UInt,
    val timeoutMs: Int,
    val stopCookie: ULong,
    val threadsTotal: UInt,
    val threadsSettled: UInt,
    val threadsParked: UInt,
    val message: String,
)

data class BridgeSingleStepRequest(
    val tid: Int,
    val flags: UInt = 0u,
)

data class BridgeSingleStepReply(
    val status: Int,
    val tid: Int,
    val flags: UInt,
    val message: String,
)

data class BridgeStopState(
    val cookie: ULong,
    val reason: UInt,
    val flags: UInt,
    val tgid: Int,
    val tid: Int,
    val eventFlags: UInt,
    val value0: ULong,
    val value1: ULong,
    val x0: ULong,
    val x1: ULong,
    val x29: ULong,
    val x30: ULong,
    val sp: ULong,
    val pc: ULong,
    val pstate: ULong,
    val features: UInt,
    val fpsr: UInt,
    val fpcr: UInt,
)

data class BridgeGetStopStateReply(
    val status: Int,
    val stop: BridgeStopState,
    val message: String,
)

data class BridgeHwpointRequest(
    val id: ULong = 0uL,
    val addr: ULong,
    val tid: Int = 0,
    val type: UInt,
    val len: UInt,
    val flags: UInt = 0u,
    val triggerHitCount: ULong = 0uL,
    val actionFlags: UInt = 0u,
)

data class BridgeHwpointRecord(
    val id: ULong,
    val addr: ULong,
    val hits: ULong,
    val triggerHitCount: ULong,
    val tgid: Int,
    val tid: Int,
    val type: UInt,
    val len: UInt,
    val flags: UInt,
    val state: UInt,
    val actionFlags: UInt,
)

data class BridgeHwpointMutationReply(
    val status: Int,
    val id: ULong,
    val addr: ULong,
    val tid: Int,
    val type: UInt,
    val len: UInt,
    val flags: UInt,
    val triggerHitCount: ULong,
    val actionFlags: UInt,
    val message: String,
)

data class BridgeHwpointListReply(
    val status: Int,
    val count: UInt,
    val message: String,
    val hwpoints: List<BridgeHwpointRecord>,
)

object BridgeWireCodec {
    fun writeFrame(output: OutputStream, command: BridgeCommand, payload: ByteArray = ByteArray(0)) {
        val header = ByteBuffer.allocate(BridgeProtocol.HEADER_SIZE)
            .order(ByteOrder.LITTLE_ENDIAN)
            .putInt(BridgeProtocol.MAGIC.toInt())
            .putInt(BridgeProtocol.VERSION.toInt())
            .putInt(command.wireId.toInt())
            .putInt(payload.size)
            .array()

        output.write(header)
        if (payload.isNotEmpty())
            output.write(payload)
        output.flush()
    }

    fun readFrame(input: InputStream): Pair<BridgeFrameHeader, ByteArray> {
        val headerBytes = readExact(input, BridgeProtocol.HEADER_SIZE)
        val headerBuffer = ByteBuffer.wrap(headerBytes).order(ByteOrder.LITTLE_ENDIAN)
        val header = BridgeFrameHeader(
            magic = headerBuffer.int.toUInt(),
            version = headerBuffer.int.toUInt(),
            command = headerBuffer.int.toUInt(),
            payloadSize = headerBuffer.int.toUInt(),
        )
        val payload = readExact(input, header.payloadSize.toInt())
        return header to payload
    }

    fun encodeSetTargetRequest(request: BridgeSetTargetRequest): ByteArray =
        ByteBuffer.allocate(BridgeProtocol.SET_TARGET_REQUEST_SIZE)
            .order(ByteOrder.LITTLE_ENDIAN)
            .putInt(request.targetPid)
            .putInt(request.targetTid)
            .array()

    fun encodeMemoryRequest(remoteAddr: ULong, length: UInt, flags: UInt = 0u): ByteArray =
        ByteBuffer.allocate(BridgeProtocol.READ_MEMORY_REQUEST_SIZE)
            .order(ByteOrder.LITTLE_ENDIAN)
            .putLong(remoteAddr.toLong())
            .putInt(length.toInt())
            .putInt(flags.toInt())
            .array()

    fun encodeMemoryWriteRequest(
        remoteAddr: ULong,
        data: ByteArray,
        flags: UInt = 0u,
    ): ByteArray =
        ByteBuffer.allocate(BridgeProtocol.WRITE_MEMORY_REQUEST_HEADER_SIZE + data.size)
            .order(ByteOrder.LITTLE_ENDIAN)
            .putLong(remoteAddr.toLong())
            .putInt(data.size)
            .putInt(flags.toInt())
            .put(data)
            .array()

    fun encodeThreadRequest(tid: Int, flags: UInt = 0u): ByteArray =
        ByteBuffer.allocate(BridgeProtocol.GET_REGISTERS_REQUEST_SIZE)
            .order(ByteOrder.LITTLE_ENDIAN)
            .putInt(tid)
            .putInt(flags.toInt())
            .array()

    fun encodePollEventsRequest(timeoutMs: Int, maxEvents: Int): ByteArray =
        ByteBuffer.allocate(BridgeProtocol.POLL_EVENTS_REQUEST_SIZE)
            .order(ByteOrder.LITTLE_ENDIAN)
            .putInt(timeoutMs)
            .putInt(maxEvents)
            .array()

    fun encodeSearchMemoryRequest(
        regionPreset: UInt,
        maxResults: UInt,
        pattern: ByteArray,
    ): ByteArray {
        require(pattern.isNotEmpty()) { "pattern must not be empty" }
        require(pattern.size <= 128) { "pattern too large: ${pattern.size}" }
        val payload = ByteBuffer.allocate(BridgeProtocol.SEARCH_MEMORY_REQUEST_SIZE)
            .order(ByteOrder.LITTLE_ENDIAN)
            .putInt(regionPreset.toInt())
            .putInt(maxResults.toInt())
            .putInt(pattern.size)
            .putInt(0)
        payload.put(pattern)
        repeat(128 - pattern.size) {
            payload.put(0)
        }
        return payload.array()
    }

    fun encodeFreezeThreadsRequest(request: BridgeFreezeThreadsRequest): ByteArray =
        ByteBuffer.allocate(BridgeProtocol.FREEZE_THREADS_REQUEST_SIZE)
            .order(ByteOrder.LITTLE_ENDIAN)
            .putInt(request.flags.toInt())
            .putInt(request.timeoutMs)
            .array()

    fun encodeContinueTargetRequest(request: BridgeContinueTargetRequest): ByteArray =
        ByteBuffer.allocate(BridgeProtocol.CONTINUE_TARGET_REQUEST_SIZE)
            .order(ByteOrder.LITTLE_ENDIAN)
            .putInt(request.flags.toInt())
            .putInt(request.timeoutMs)
            .putLong(request.stopCookie.toLong())
            .array()

    fun encodeSingleStepRequest(request: BridgeSingleStepRequest): ByteArray =
        ByteBuffer.allocate(BridgeProtocol.SINGLE_STEP_REQUEST_SIZE)
            .order(ByteOrder.LITTLE_ENDIAN)
            .putInt(request.tid)
            .putInt(request.flags.toInt())
            .array()

    fun encodeHwpointRequest(request: BridgeHwpointRequest): ByteArray =
        ByteBuffer.allocate(BridgeProtocol.HWPOINT_REQUEST_SIZE)
            .order(ByteOrder.LITTLE_ENDIAN)
            .putLong(request.id.toLong())
            .putLong(request.addr.toLong())
            .putInt(request.tid)
            .putInt(request.type.toInt())
            .putInt(request.len.toInt())
            .putInt(request.flags.toInt())
            .putLong(request.triggerHitCount.toLong())
            .putInt(request.actionFlags.toInt())
            .putInt(0)
            .array()

    fun decodeHelloReply(payload: ByteArray): BridgeHelloReply {
        val buffer = payloadBuffer(payload, BridgeProtocol.HELLO_REPLY_SIZE)
        val status = buffer.int
        val version = buffer.int.toUInt()
        val features = buffer.long.toULong()
        val message = decodeCString(buffer, 64)
        return BridgeHelloReply(status, version, features, message)
    }

    fun decodeOpenSessionReply(payload: ByteArray): BridgeOpenSessionReply {
        val buffer = payloadBuffer(payload, BridgeProtocol.OPEN_SESSION_REPLY_SIZE)
        val status = buffer.int
        val sessionOpen = buffer.int != 0
        val sessionId = buffer.long.toULong()
        val message = decodeCString(buffer, 64)
        return BridgeOpenSessionReply(status, sessionOpen, sessionId, message)
    }

    fun decodeSetTargetReply(payload: ByteArray): BridgeSetTargetReply {
        val buffer = payloadBuffer(payload, BridgeProtocol.SET_TARGET_REPLY_SIZE)
        val status = buffer.int
        val targetPid = buffer.int
        val targetTid = buffer.int
        val message = decodeCString(buffer, 60)
        return BridgeSetTargetReply(status, targetPid, targetTid, message)
    }

    fun decodeStatusSnapshot(payload: ByteArray): BridgeStatusSnapshot {
        val buffer = payloadBuffer(payload, BridgeProtocol.STATUS_SNAPSHOT_REPLY_SIZE)
        val status = buffer.int
        val connected = buffer.int != 0
        val targetPid = buffer.int
        val targetTid = buffer.int
        val sessionOpen = buffer.int != 0
        val agentPid = buffer.int
        val ownerPid = buffer.int
        val hookActive = buffer.int
        val eventQueueDepth = buffer.int.toUInt()
        val sessionId = buffer.long.toULong()
        val transport = decodeCString(buffer, 32)
        val message = decodeCString(buffer, 64)
        return BridgeStatusSnapshot(
            status = status,
            connected = connected,
            targetPid = targetPid,
            targetTid = targetTid,
            sessionOpen = sessionOpen,
            agentPid = agentPid,
            ownerPid = ownerPid,
            hookActive = hookActive,
            eventQueueDepth = eventQueueDepth,
            sessionId = sessionId,
            transport = transport,
            message = message,
        )
    }

    fun decodeQueryProcessesReply(payload: ByteArray): BridgeProcessListReply {
        val buffer = payloadBuffer(payload, BridgeProtocol.QUERY_PROCESSES_REPLY_HEADER_SIZE)
        val status = buffer.int
        val count = buffer.int.toUInt()
        val message = decodeCString(buffer, 64)
        val remaining = payload.size - BridgeProtocol.QUERY_PROCESSES_REPLY_HEADER_SIZE
        require(remaining >= count.toInt() * BridgeProtocol.QUERY_PROCESS_RECORD_SIZE) {
            "process payload too small: got=${payload.size} count=$count"
        }

        val processes = ArrayList<BridgeProcessRecord>(count.toInt())
        repeat(count.toInt()) {
            val pid = buffer.int
            val uid = buffer.int
            val comm = decodeCString(buffer, 64)
            val cmdline = decodeCString(buffer, 128)
            processes += BridgeProcessRecord(
                pid = pid,
                uid = uid,
                comm = comm,
                cmdline = cmdline,
            )
        }

        return BridgeProcessListReply(
            status = status,
            count = count,
            message = message,
            processes = processes,
        )
    }

    fun decodeReadMemoryReply(payload: ByteArray): BridgeMemoryReadReply {
        val buffer = payloadBuffer(payload, BridgeProtocol.READ_MEMORY_REPLY_HEADER_SIZE)
        val status = buffer.int
        val bytesDone = buffer.int.toUInt()
        val remoteAddr = buffer.long.toULong()
        val requestedLength = buffer.int.toUInt()
        val flags = buffer.int.toUInt()
        val message = decodeCString(buffer, 64)
        val data = ByteArray(payload.size - BridgeProtocol.READ_MEMORY_REPLY_HEADER_SIZE)
        buffer.get(data)
        return BridgeMemoryReadReply(
            status = status,
            remoteAddr = remoteAddr,
            requestedLength = requestedLength,
            flags = flags,
            bytesDone = bytesDone,
            message = message,
            data = data,
        )
    }

    fun decodeWriteMemoryReply(payload: ByteArray): BridgeMemoryWriteReply {
        val buffer = payloadBuffer(payload, BridgeProtocol.WRITE_MEMORY_REPLY_SIZE)
        return BridgeMemoryWriteReply(
            status = buffer.int,
            bytesDone = buffer.int.toUInt(),
            remoteAddr = buffer.long.toULong(),
            requestedLength = buffer.int.toUInt(),
            flags = buffer.int.toUInt(),
            message = decodeCString(buffer, 64),
        )
    }

    fun decodeQueryThreadsReply(payload: ByteArray): BridgeThreadListReply {
        val buffer = payloadBuffer(payload, BridgeProtocol.QUERY_THREADS_REPLY_HEADER_SIZE)
        val status = buffer.int
        val count = buffer.int.toUInt()
        val nextTid = buffer.int
        val done = buffer.int != 0
        val message = decodeCString(buffer, 64)
        val remaining = payload.size - BridgeProtocol.QUERY_THREADS_REPLY_HEADER_SIZE
        require(remaining >= count.toInt() * BridgeProtocol.QUERY_THREAD_RECORD_SIZE) {
            "thread payload too small: got=${payload.size} count=$count"
        }

        val threads = ArrayList<BridgeThreadRecord>(count.toInt())
        repeat(count.toInt()) {
            val tid = buffer.int
            val tgid = buffer.int
            val flags = buffer.int.toUInt()
            buffer.int
            threads += BridgeThreadRecord(
                tid = tid,
                tgid = tgid,
                flags = flags,
                userPc = buffer.long.toULong(),
                userSp = buffer.long.toULong(),
                comm = decodeCString(buffer, 16),
            )
        }

        return BridgeThreadListReply(
            status = status,
            count = count,
            done = done,
            nextTid = nextTid,
            message = message,
            threads = threads,
        )
    }

    fun decodeGetRegistersReply(payload: ByteArray): BridgeThreadRegistersReply {
        val buffer = payloadBuffer(payload, BridgeProtocol.GET_REGISTERS_REPLY_SIZE)
        val status = buffer.int
        val tid = buffer.int
        val flags = buffer.int.toUInt()
        buffer.int
        val regs = ULongArray(31) { buffer.long.toULong() }
        val sp = buffer.long.toULong()
        val pc = buffer.long.toULong()
        val pstate = buffer.long.toULong()
        val features = buffer.int.toUInt()
        val fpsr = buffer.int.toUInt()
        val fpcr = buffer.int.toUInt()
        buffer.int
        val v0Lo = buffer.long.toULong()
        val v0Hi = buffer.long.toULong()
        val message = decodeCString(buffer, 64)
        return BridgeThreadRegistersReply(
            status = status,
            tid = tid,
            flags = flags,
            regs = regs,
            sp = sp,
            pc = pc,
            pstate = pstate,
            features = features,
            fpsr = fpsr,
            fpcr = fpcr,
            v0Lo = v0Lo,
            v0Hi = v0Hi,
            message = message,
        )
    }

    fun decodePollEventsReply(payload: ByteArray): BridgeEventBatchReply {
        val buffer = payloadBuffer(payload, BridgeProtocol.POLL_EVENTS_REPLY_HEADER_SIZE)
        val status = buffer.int
        val count = buffer.int.toUInt()
        val message = decodeCString(buffer, 64)
        val remaining = payload.size - BridgeProtocol.POLL_EVENTS_REPLY_HEADER_SIZE
        require(remaining >= count.toInt() * BridgeProtocol.POLL_EVENT_RECORD_SIZE) {
            "event payload too small: got=${payload.size} count=$count"
        }

        val events = ArrayList<BridgeEventRecord>(count.toInt())
        repeat(count.toInt()) {
            events += BridgeEventRecord(
                version = buffer.int.toUInt(),
                type = buffer.int.toUInt(),
                size = buffer.int.toUInt(),
                code = buffer.int.toUInt(),
                sessionId = buffer.long.toULong(),
                seq = buffer.long.toULong(),
                tgid = buffer.int,
                tid = buffer.int,
                flags = buffer.int.toUInt(),
                value0 = run {
                    buffer.int
                    buffer.long.toULong()
                },
                value1 = buffer.long.toULong(),
            )
        }

        return BridgeEventBatchReply(
            status = status,
            count = count,
            message = message,
            events = events,
        )
    }

    fun decodeQueryImagesReply(payload: ByteArray): BridgeImageListReply {
        val buffer = payloadBuffer(payload, BridgeProtocol.QUERY_IMAGES_REPLY_HEADER_SIZE)
        val status = buffer.int
        val count = buffer.int.toUInt()
        val message = decodeCString(buffer, 64)
        val remaining = payload.size - BridgeProtocol.QUERY_IMAGES_REPLY_HEADER_SIZE
        require(remaining >= count.toInt() * BridgeProtocol.QUERY_IMAGE_RECORD_SIZE) {
            "image payload too small: got=${payload.size} count=$count"
        }

        val images = ArrayList<BridgeImageRecord>(count.toInt())
        repeat(count.toInt()) {
            images += BridgeImageRecord(
                startAddr = buffer.long.toULong(),
                endAddr = buffer.long.toULong(),
                baseAddr = buffer.long.toULong(),
                pgoff = buffer.long.toULong(),
                inode = buffer.long.toULong(),
                prot = buffer.int.toUInt(),
                flags = buffer.int.toUInt(),
                devMajor = buffer.int.toUInt(),
                devMinor = buffer.int.toUInt(),
                segmentCount = buffer.int.toUInt(),
                name = run {
                    buffer.int
                    decodeCString(buffer, 256)
                },
            )
        }

        return BridgeImageListReply(
            status = status,
            count = count,
            message = message,
            images = images,
        )
    }

    fun decodeQueryVmasReply(payload: ByteArray): BridgeVmaListReply {
        val buffer = payloadBuffer(payload, BridgeProtocol.QUERY_VMAS_REPLY_HEADER_SIZE)
        val status = buffer.int
        val count = buffer.int.toUInt()
        val message = decodeCString(buffer, 64)
        val remaining = payload.size - BridgeProtocol.QUERY_VMAS_REPLY_HEADER_SIZE
        require(remaining >= count.toInt() * BridgeProtocol.QUERY_VMA_RECORD_SIZE) {
            "vma payload too small: got=${payload.size} count=$count"
        }

        val vmas = ArrayList<BridgeVmaRecord>(count.toInt())
        repeat(count.toInt()) {
            vmas += BridgeVmaRecord(
                startAddr = buffer.long.toULong(),
                endAddr = buffer.long.toULong(),
                pgoff = buffer.long.toULong(),
                inode = buffer.long.toULong(),
                vmFlagsRaw = buffer.long.toULong(),
                prot = buffer.int.toUInt(),
                flags = buffer.int.toUInt(),
                devMajor = buffer.int.toUInt(),
                devMinor = buffer.int.toUInt(),
                name = run {
                    buffer.int
                    buffer.int
                    decodeCString(buffer, 256)
                },
            )
        }

        return BridgeVmaListReply(
            status = status,
            count = count,
            message = message,
            vmas = vmas,
        )
    }

    fun decodeSearchMemoryReply(payload: ByteArray): BridgeMemorySearchReply {
        val buffer = payloadBuffer(payload, BridgeProtocol.SEARCH_MEMORY_REPLY_HEADER_SIZE)
        val status = buffer.int
        val count = buffer.int.toUInt()
        val searchedVmaCount = buffer.int.toUInt()
        buffer.int
        val scannedBytes = buffer.long.toULong()
        val message = decodeCString(buffer, 64)
        val remaining = payload.size - BridgeProtocol.SEARCH_MEMORY_REPLY_HEADER_SIZE
        require(remaining >= count.toInt() * BridgeProtocol.SEARCH_MEMORY_RESULT_RECORD_SIZE) {
            "search payload too small: got=${payload.size} count=$count"
        }

        val results = ArrayList<BridgeMemorySearchRecord>(count.toInt())
        repeat(count.toInt()) {
            val address = buffer.long.toULong()
            val regionStart = buffer.long.toULong()
            val regionEnd = buffer.long.toULong()
            val previewSize = buffer.int.toUInt()
            buffer.int
            val preview = ByteArray(32)
            buffer.get(preview)
            results += BridgeMemorySearchRecord(
                address = address,
                regionStart = regionStart,
                regionEnd = regionEnd,
                previewSize = previewSize,
                preview = preview,
            )
        }

        return BridgeMemorySearchReply(
            status = status,
            count = count,
            searchedVmaCount = searchedVmaCount,
            scannedBytes = scannedBytes,
            message = message,
            results = results,
        )
    }

    fun decodeFreezeThreadsReply(payload: ByteArray): BridgeFreezeThreadsReply {
        val buffer = payloadBuffer(payload, BridgeProtocol.FREEZE_THREADS_REPLY_SIZE)
        return BridgeFreezeThreadsReply(
            status = buffer.int,
            flags = buffer.int.toUInt(),
            timeoutMs = buffer.int,
            threadsTotal = buffer.int.toUInt(),
            threadsSettled = buffer.int.toUInt(),
            threadsParked = buffer.int.toUInt(),
            message = decodeCString(buffer, 64),
        )
    }

    fun decodeContinueTargetReply(payload: ByteArray): BridgeContinueTargetReply {
        val buffer = payloadBuffer(payload, BridgeProtocol.CONTINUE_TARGET_REPLY_SIZE)
        return BridgeContinueTargetReply(
            status = buffer.int,
            flags = buffer.int.toUInt(),
            timeoutMs = buffer.int,
            stopCookie = buffer.long.toULong(),
            threadsTotal = buffer.int.toUInt(),
            threadsSettled = buffer.int.toUInt(),
            threadsParked = buffer.int.toUInt(),
            message = decodeCString(buffer, 64),
        )
    }

    fun decodeSingleStepReply(payload: ByteArray): BridgeSingleStepReply {
        val buffer = payloadBuffer(payload, BridgeProtocol.SINGLE_STEP_REPLY_SIZE)
        return BridgeSingleStepReply(
            status = buffer.int,
            tid = buffer.int,
            flags = buffer.int.toUInt(),
            message = decodeCString(buffer, 64),
        )
    }

    fun decodeGetStopStateReply(payload: ByteArray): BridgeGetStopStateReply {
        val buffer = payloadBuffer(payload, BridgeProtocol.GET_STOP_STATE_REPLY_SIZE)
        val status = buffer.int
        val stop = BridgeStopState(
            cookie = buffer.long.toULong(),
            reason = buffer.int.toUInt(),
            flags = buffer.int.toUInt(),
            tgid = buffer.int,
            tid = buffer.int,
            eventFlags = buffer.int.toUInt(),
            value0 = buffer.long.toULong(),
            value1 = buffer.long.toULong(),
            x0 = buffer.long.toULong(),
            x1 = buffer.long.toULong(),
            x29 = buffer.long.toULong(),
            x30 = buffer.long.toULong(),
            sp = buffer.long.toULong(),
            pc = buffer.long.toULong(),
            pstate = buffer.long.toULong(),
            features = buffer.int.toUInt(),
            fpsr = buffer.int.toUInt(),
            fpcr = buffer.int.toUInt(),
        )
        val message = decodeCString(buffer, 64)
        return BridgeGetStopStateReply(
            status = status,
            stop = stop,
            message = message,
        )
    }

    fun decodeHwpointMutationReply(payload: ByteArray): BridgeHwpointMutationReply {
        val buffer = payloadBuffer(payload, BridgeProtocol.HWPOINT_MUTATION_REPLY_SIZE)
        return BridgeHwpointMutationReply(
            status = buffer.int,
            id = buffer.long.toULong(),
            addr = buffer.long.toULong(),
            tid = buffer.int,
            type = buffer.int.toUInt(),
            len = buffer.int.toUInt(),
            flags = buffer.int.toUInt(),
            triggerHitCount = buffer.long.toULong(),
            actionFlags = buffer.int.toUInt(),
            message = decodeCString(buffer, 64),
        )
    }

    fun decodeQueryHwpointsReply(payload: ByteArray): BridgeHwpointListReply {
        val buffer = payloadBuffer(payload, BridgeProtocol.QUERY_HWPOINTS_REPLY_HEADER_SIZE)
        val status = buffer.int
        val count = buffer.int.toUInt()
        val message = decodeCString(buffer, 64)
        val remaining = payload.size - BridgeProtocol.QUERY_HWPOINTS_REPLY_HEADER_SIZE
        require(remaining >= count.toInt() * BridgeProtocol.QUERY_HWPOINT_RECORD_SIZE) {
            "hwpoint payload too small: got=${payload.size} count=$count"
        }

        val hwpoints = ArrayList<BridgeHwpointRecord>(count.toInt())
        repeat(count.toInt()) {
            val record = BridgeHwpointRecord(
                id = buffer.long.toULong(),
                addr = buffer.long.toULong(),
                hits = buffer.long.toULong(),
                triggerHitCount = buffer.long.toULong(),
                tgid = buffer.int,
                tid = buffer.int,
                type = buffer.int.toUInt(),
                len = buffer.int.toUInt(),
                flags = buffer.int.toUInt(),
                state = buffer.int.toUInt(),
                actionFlags = buffer.int.toUInt(),
            )
            buffer.int
            hwpoints += record
        }

        return BridgeHwpointListReply(
            status = status,
            count = count,
            message = message,
            hwpoints = hwpoints,
        )
    }

    private fun payloadBuffer(payload: ByteArray, minimumSize: Int): ByteBuffer {
        require(payload.size >= minimumSize) {
            "payload too small: got=${payload.size} need=$minimumSize"
        }
        return ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
    }

    private fun decodeCString(buffer: ByteBuffer, size: Int): String {
        val bytes = ByteArray(size)
        buffer.get(bytes)
        val zero = bytes.indexOf(0)
        val length = if (zero >= 0) zero else bytes.size
        return String(bytes, 0, length, StandardCharsets.UTF_8)
    }

    private fun readExact(input: InputStream, size: Int): ByteArray {
        val out = ByteArray(size)
        var done = 0
        while (done < size) {
            val nr = input.read(out, done, size - done)
            if (nr < 0)
                throw EOFException("unexpected eof after $done/$size bytes")
            done += nr
        }
        return out
    }
}
