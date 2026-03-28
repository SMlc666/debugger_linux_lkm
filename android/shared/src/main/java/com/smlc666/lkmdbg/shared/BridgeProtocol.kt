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

data class DashboardProcess(
    val pid: Int,
    val name: String,
    val packageName: String,
    val arch: String,
    val state: String,
)

data class DashboardThread(
    val tid: Int,
    val name: String,
    val pc: String,
    val state: String,
)

data class DashboardEvent(
    val title: String,
    val detail: String,
    val level: String,
    val timestamp: String,
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
