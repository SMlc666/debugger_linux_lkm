package com.smlc666.lkmdbg.shared

object BridgeProtocol {
    const val VERSION: UInt = 1u
    const val MAGIC: UInt = 0x4C4B4D44u
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
}

data class BridgeFrameHeader(
    val magic: UInt = BridgeProtocol.MAGIC,
    val version: UInt = BridgeProtocol.VERSION,
    val command: UInt,
    val payloadSize: UInt,
)

data class BridgeHello(
    val clientVersion: UInt = BridgeProtocol.VERSION,
    val clientName: String = "lkmdbg-android",
)

data class BridgeHelloReply(
    val status: BridgeStatusCode,
    val serverVersion: UInt,
    val featureBits: ULong,
    val message: String,
)

data class BridgeStatusSnapshot(
    val connected: Boolean,
    val targetPid: Int,
    val targetTid: Int,
    val sessionOpen: Boolean,
    val agentPid: Int,
    val transport: String,
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
