package com.smlc666.lkmdbg.data

data class MemoryPreviewRow(
    val address: ULong,
    val byteValues: List<Int>,
    val hexBytes: String,
    val ascii: String,
)

data class MemoryScalarValue(
    val label: String,
    val value: String,
)

data class MemoryRegionSummary(
    val name: String,
    val startAddr: ULong,
    val endAddr: ULong,
    val prot: UInt,
    val flags: UInt,
)

data class MemoryPage(
    val focusAddress: ULong,
    val pageStart: ULong,
    val pageSize: UInt,
    val bytes: ByteArray,
    val rows: List<MemoryPreviewRow>,
    val disassembly: List<String>,
    val scalars: List<MemoryScalarValue>,
    val region: MemoryRegionSummary?,
)
