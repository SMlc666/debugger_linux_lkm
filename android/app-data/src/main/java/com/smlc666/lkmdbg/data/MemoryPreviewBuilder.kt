package com.smlc666.lkmdbg.data

import com.smlc666.lkmdbg.shared.BridgeVmaRecord

internal class MemoryPreviewBuilder(
    private val pageSize: UInt,
    private val rowBytes: Int,
) {
    fun buildPage(
        focusAddr: ULong,
        pageStart: ULong,
        bytes: ByteArray,
        vmas: List<BridgeVmaRecord>,
    ): MemoryPage {
        val rows = ArrayList<MemoryPreviewRow>((bytes.size + rowBytes - 1) / rowBytes)
        var offset = 0

        while (offset < bytes.size) {
            val end = minOf(bytes.size, offset + rowBytes)
            val slice = bytes.copyOfRange(offset, end)
            val hexBytes = slice.joinToString(" ") { "%02x".format(it.toInt() and 0xff) }
            val ascii = buildString(slice.size) {
                slice.forEach { byte ->
                    val value = byte.toInt() and 0xff
                    append(if (value in 0x20..0x7e) value.toChar() else '.')
                }
            }
            rows += MemoryPreviewRow(
                address = pageStart + offset.toUInt().toULong(),
                byteValues = slice.map { it.toInt() and 0xff },
                hexBytes = hexBytes,
                ascii = ascii,
            )
            offset = end
        }

        val disassembly = runCatching {
            NativeDisassembler.disassembleArm64(pageStart, bytes)
        }.getOrDefault(emptyList())

        return MemoryPage(
            focusAddress = focusAddr,
            pageStart = pageStart,
            pageSize = pageSize,
            bytes = bytes,
            rows = rows,
            disassembly = disassembly,
            scalars = buildScalarValues(focusAddr, pageStart, bytes),
            region = vmas.firstOrNull { focusAddr >= it.startAddr && focusAddr < it.endAddr }?.let { vma ->
                MemoryRegionSummary(
                    name = vma.name,
                    startAddr = vma.startAddr,
                    endAddr = vma.endAddr,
                    prot = vma.prot,
                    flags = vma.flags,
                )
            },
        )
    }

    fun retargetPage(page: MemoryPage, remoteAddr: ULong): MemoryPage =
        page.copy(
            focusAddress = remoteAddr,
            scalars = buildScalarValues(remoteAddr, page.pageStart, page.bytes),
        )

    fun addressInsidePage(page: MemoryPage, remoteAddr: ULong): Boolean {
        val end = page.pageStart + page.bytes.size.toUInt().toULong()
        return remoteAddr >= page.pageStart && remoteAddr < end
    }

    fun alignDown(address: ULong): ULong {
        val mask = pageSize.toULong() - 1uL
        return address and mask.inv()
    }

    fun selectionBytes(page: MemoryPage, size: Int): ByteArray? {
        if (page.focusAddress < page.pageStart)
            return null
        val offset = (page.focusAddress - page.pageStart).toInt()
        if (offset >= page.bytes.size)
            return null
        val clampedSize = size.coerceAtLeast(1)
        val end = minOf(page.bytes.size, offset + clampedSize)
        return page.bytes.copyOfRange(offset, end)
    }

    private fun buildScalarValues(
        focusAddr: ULong,
        pageStart: ULong,
        bytes: ByteArray,
    ): List<MemoryScalarValue> {
        if (focusAddr < pageStart)
            return emptyList()
        val offset = (focusAddr - pageStart).toInt()
        if (offset >= bytes.size)
            return emptyList()

        val remaining = bytes.size - offset
        val scalarBytes = bytes.copyOfRange(offset, bytes.size)
        return buildList {
            add(MemoryScalarValue("u8", (scalarBytes[0].toInt() and 0xff).toString()))
            add(MemoryScalarValue("i32", MemoryValueCodec.scalarInt32(scalarBytes, remaining) ?: "n/a"))
            add(MemoryScalarValue("i64", MemoryValueCodec.scalarInt64(scalarBytes, remaining) ?: "n/a"))
            add(MemoryScalarValue("f32", MemoryValueCodec.scalarFloat32(scalarBytes, remaining) ?: "n/a"))
            add(MemoryScalarValue("f64", MemoryValueCodec.scalarFloat64(scalarBytes, remaining) ?: "n/a"))
        }
    }
}
