package com.smlc666.lkmdbg.data

import com.smlc666.lkmdbg.shared.BridgeVmaRecord

private const val LKMDBG_VMA_PROT_READ = 0x00000001
private const val LKMDBG_VMA_PROT_WRITE = 0x00000002
private const val LKMDBG_VMA_PROT_EXEC = 0x00000004

private const val LKMDBG_VMA_FLAG_ANON = 0x00000001
private const val LKMDBG_VMA_FLAG_FILE = 0x00000002
private const val LKMDBG_VMA_FLAG_STACK = 0x00000008
private const val LKMDBG_VMA_FLAG_HEAP = 0x00000010

enum class MemorySearchValueType {
    Int32,
    Int64,
    Float32,
    Float64,
    HexBytes,
    Ascii,
}

enum class MemorySearchRefineMode {
    Exact,
    Changed,
    Unchanged,
    Increased,
    Decreased,
}

enum class MemoryRegionPreset(val wireValue: UInt) {
    All(0u),
    Xa(1u),
    Cd(2u),
    Ca(3u),
    Ch(4u),
    Stack(5u);

    fun matches(vma: BridgeVmaRecord): Boolean {
        val flags = vma.flags.toInt()
        val prot = vma.prot.toInt()
        val readable = (prot and LKMDBG_VMA_PROT_READ) != 0
        val writable = (prot and LKMDBG_VMA_PROT_WRITE) != 0
        val executable = (prot and LKMDBG_VMA_PROT_EXEC) != 0
        val anon = (flags and LKMDBG_VMA_FLAG_ANON) != 0
        val file = (flags and LKMDBG_VMA_FLAG_FILE) != 0 || (!anon && vma.name.isNotBlank())
        val heap = (flags and LKMDBG_VMA_FLAG_HEAP) != 0
        val stack = (flags and LKMDBG_VMA_FLAG_STACK) != 0

        if (!readable)
            return false

        return when (this) {
            All -> true
            Xa -> executable && file
            Cd -> file && writable && !executable
            Ca -> anon && writable && !heap && !stack && !executable
            Ch -> heap
            Stack -> stack
        }
    }
}

data class MemorySearchResult(
    val address: ULong,
    val regionName: String,
    val regionStart: ULong,
    val regionEnd: ULong,
    val matchSize: Int,
    val previewBytes: ByteArray,
    val previewHex: String,
    val valueSummary: String,
)

data class MemorySearchOutcome(
    val searchedVmaCount: Int,
    val scannedBytes: ULong,
    val results: List<MemorySearchResult>,
)
