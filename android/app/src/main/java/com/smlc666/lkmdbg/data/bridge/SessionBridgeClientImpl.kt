package com.smlc666.lkmdbg.data.bridge

import com.smlc666.lkmdbg.data.PipeAgentClient
import com.smlc666.lkmdbg.shared.BridgeHelloReply
import com.smlc666.lkmdbg.shared.BridgeOpenSessionReply
import com.smlc666.lkmdbg.shared.BridgeStatusSnapshot

class SessionBridgeClientImpl(
    private val pipeAgentClient: PipeAgentClient,
) : SessionBridgeClient {
    override suspend fun connect(): BridgeHelloReply = pipeAgentClient.connect()

    override suspend fun openSession(): BridgeOpenSessionReply = pipeAgentClient.openSession()

    override suspend fun statusSnapshot(): BridgeStatusSnapshot = pipeAgentClient.statusSnapshot()
}
