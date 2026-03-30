package com.smlc666.lkmdbg.data

data class RootBridgeDiagnostics(
    val uid: Int,
    val agentPath: String,
    val suCandidates: List<String>,
)
