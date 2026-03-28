package com.smlc666.lkmdbg.data

data class MemoryPreviewRow(
    val address: ULong,
    val hexBytes: String,
    val ascii: String,
)

data class MemoryPreview(
    val address: ULong,
    val bytes: ByteArray,
    val rows: List<MemoryPreviewRow>,
    val disassembly: List<String>,
)
