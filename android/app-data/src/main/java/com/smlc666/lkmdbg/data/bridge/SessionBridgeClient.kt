package com.smlc666.lkmdbg.data.bridge

import com.smlc666.lkmdbg.shared.BridgeHelloReply
import com.smlc666.lkmdbg.shared.BridgeOpenSessionReply
import com.smlc666.lkmdbg.shared.BridgeStatusSnapshot

interface SessionBridgeClient {
    suspend fun connect(): BridgeHelloReply

    suspend fun openSession(): BridgeOpenSessionReply

    suspend fun statusSnapshot(): BridgeStatusSnapshot
}
