package com.smlc666.lkmdbg.data

import com.smlc666.lkmdbg.shared.BridgeMemoryReadReply
import com.smlc666.lkmdbg.shared.BridgeVmaRecord
import java.io.BufferedOutputStream
import java.io.File
import java.io.FileOutputStream
import java.io.RandomAccessFile

private data class MemorySearchSnapshotSegment(
    val startAddr: ULong,
    val endAddr: ULong,
    val fileOffset: Long,
    val length: Int,
)

private data class MemorySearchSnapshot(
    val targetPid: Int,
    val regionPreset: MemoryRegionPreset,
    val file: File,
    val rangeCount: Int,
    val totalBytes: ULong,
    val segments: List<MemorySearchSnapshotSegment>,
)

internal data class CapturedMemorySearchSnapshot(
    val rangeCount: Int,
    val totalBytes: ULong,
)

internal class MemorySearchSnapshotController(
    private val cacheDir: File,
    private val snapshotChunkSize: UInt,
) {
    private var snapshot: MemorySearchSnapshot? = null

    fun supportsUnknownInitial(valueType: MemorySearchValueType): Boolean =
        when (valueType) {
            MemorySearchValueType.Int32,
            MemorySearchValueType.Int64,
            MemorySearchValueType.Float32,
            MemorySearchValueType.Float64 -> true

            MemorySearchValueType.HexBytes,
            MemorySearchValueType.Ascii -> false
        }

    fun discard() {
        snapshot?.file?.delete()
        snapshot = null
    }

    suspend fun capture(
        targetPid: Int,
        regionPreset: MemoryRegionPreset,
        matchingVmas: List<BridgeVmaRecord>,
        readMemory: suspend (ULong, UInt) -> BridgeMemoryReadReply,
    ): CapturedMemorySearchSnapshot {
        discard()

        val snapshotFile = File.createTempFile("lkmdbg-search-", ".bin", cacheDir)
        val segments = ArrayList<MemorySearchSnapshotSegment>()
        var totalBytes = 0uL
        var fileOffset = 0L

        try {
            BufferedOutputStream(FileOutputStream(snapshotFile)).use { output ->
                matchingVmas.forEach { vma ->
                    var cursor = vma.startAddr
                    while (cursor < vma.endAddr) {
                        val remaining = vma.endAddr - cursor
                        val requestSize = minOf(
                            remaining,
                            snapshotChunkSize.toULong(),
                        ).toUInt()
                        val reply = readMemory(cursor, requestSize)
                        if (reply.status != 0) {
                            throw IllegalStateException(
                                reply.message.ifBlank { "snapshot read failed status=${reply.status}" },
                            )
                        }
                        val bytesDone = reply.bytesDone.toInt()
                        if (bytesDone <= 0)
                            break
                        val chunk = reply.data.copyOf(bytesDone)
                        output.write(chunk)
                        segments += MemorySearchSnapshotSegment(
                            startAddr = cursor,
                            endAddr = cursor + bytesDone.toUInt().toULong(),
                            fileOffset = fileOffset,
                            length = bytesDone,
                        )
                        fileOffset += bytesDone.toLong()
                        totalBytes += bytesDone.toULong()
                        cursor += bytesDone.toUInt().toULong()
                    }
                }
            }

            snapshot = MemorySearchSnapshot(
                targetPid = targetPid,
                regionPreset = regionPreset,
                file = snapshotFile,
                rangeCount = matchingVmas.size,
                totalBytes = totalBytes,
                segments = segments,
            )
            return CapturedMemorySearchSnapshot(
                rangeCount = matchingVmas.size,
                totalBytes = totalBytes,
            )
        } catch (t: Throwable) {
            snapshotFile.delete()
            snapshot = null
            throw t
        }
    }

    suspend fun refine(
        targetPid: Int,
        regionPreset: MemoryRegionPreset,
        valueType: MemorySearchValueType,
        refineMode: MemorySearchRefineMode,
        vmas: List<BridgeVmaRecord>,
        readMemory: suspend (ULong, UInt) -> BridgeMemoryReadReply,
    ): MemorySearchOutcome {
        val activeSnapshot = snapshot
            ?: throw IllegalStateException("missing snapshot")
        if (activeSnapshot.targetPid != targetPid || activeSnapshot.regionPreset != regionPreset) {
            discard()
            throw IllegalStateException("missing snapshot")
        }

        val width = unknownInitialWidth(valueType)
        val results = ArrayList<MemorySearchResult>(128)
        var scannedBytes = 0uL

        RandomAccessFile(activeSnapshot.file, "r").use { input ->
            activeSnapshot.segments.forEach { segment ->
                val reply = readMemory(segment.startAddr, segment.length.toUInt())
                if (reply.status != 0) {
                    throw IllegalStateException(
                        reply.message.ifBlank { "snapshot refine read failed status=${reply.status}" },
                    )
                }
                val bytesDone = minOf(reply.bytesDone.toInt(), segment.length)
                if (bytesDone < width)
                    return@forEach

                val previous = ByteArray(bytesDone)
                input.seek(segment.fileOffset)
                input.readFully(previous)
                val current = reply.data.copyOf(bytesDone)
                scannedBytes += bytesDone.toULong()

                var offset = 0
                while (offset + width <= bytesDone && results.size < 512) {
                    val previousValue = previous.copyOfRange(offset, offset + width)
                    val currentValue = current.copyOfRange(offset, offset + width)
                    if (matchesUnknownInitialRefine(
                            valueType,
                            refineMode,
                            previousValue,
                            currentValue,
                        )
                    ) {
                        val previewEnd = minOf(bytesDone, offset + 16)
                        results += buildSnapshotSearchResult(
                            address = segment.startAddr + offset.toUInt().toULong(),
                            regionStart = segment.startAddr,
                            regionEnd = segment.endAddr,
                            preview = current.copyOfRange(offset, previewEnd),
                            matchSize = width,
                            valueType = valueType,
                            vmas = vmas,
                        )
                    }
                    offset += width
                }
            }
        }

        return MemorySearchOutcome(
            searchedVmaCount = activeSnapshot.rangeCount,
            scannedBytes = scannedBytes,
            results = results,
        )
    }

    private fun unknownInitialWidth(valueType: MemorySearchValueType): Int =
        when (valueType) {
            MemorySearchValueType.Int32, MemorySearchValueType.Float32 -> 4
            MemorySearchValueType.Int64, MemorySearchValueType.Float64 -> 8
            MemorySearchValueType.HexBytes, MemorySearchValueType.Ascii ->
                throw IllegalArgumentException("unknown initial unsupported for variable-width types")
        }

    private fun matchesUnknownInitialRefine(
        valueType: MemorySearchValueType,
        refineMode: MemorySearchRefineMode,
        previous: ByteArray,
        current: ByteArray,
    ): Boolean = when (refineMode) {
        MemorySearchRefineMode.Exact -> false
        MemorySearchRefineMode.Changed -> !current.contentEquals(previous)
        MemorySearchRefineMode.Unchanged -> current.contentEquals(previous)
        MemorySearchRefineMode.Increased -> compareNumericSearchValue(valueType, current, previous) > 0
        MemorySearchRefineMode.Decreased -> compareNumericSearchValue(valueType, current, previous) < 0
    }

    private fun compareNumericSearchValue(
        valueType: MemorySearchValueType,
        current: ByteArray,
        previous: ByteArray,
    ): Int = when (valueType) {
        MemorySearchValueType.Int32 ->
            MemoryValueCodec.scalarInt32Bits(current, current.size)!!
                .compareTo(MemoryValueCodec.scalarInt32Bits(previous, previous.size)!!)

        MemorySearchValueType.Int64 ->
            MemoryValueCodec.scalarInt64Bits(current, current.size)!!
                .compareTo(MemoryValueCodec.scalarInt64Bits(previous, previous.size)!!)

        MemorySearchValueType.Float32 ->
            Float.fromBits(MemoryValueCodec.scalarInt32Bits(current, current.size)!!)
                .compareTo(Float.fromBits(MemoryValueCodec.scalarInt32Bits(previous, previous.size)!!))

        MemorySearchValueType.Float64 ->
            Double.fromBits(MemoryValueCodec.scalarInt64Bits(current, current.size)!!)
                .compareTo(Double.fromBits(MemoryValueCodec.scalarInt64Bits(previous, previous.size)!!))

        MemorySearchValueType.HexBytes, MemorySearchValueType.Ascii ->
            throw IllegalArgumentException("numeric compare requires numeric value type")
    }

    private fun buildSnapshotSearchResult(
        address: ULong,
        regionStart: ULong,
        regionEnd: ULong,
        preview: ByteArray,
        matchSize: Int,
        valueType: MemorySearchValueType,
        vmas: List<BridgeVmaRecord>,
    ): MemorySearchResult {
        val region = vmas.firstOrNull { address >= it.startAddr && address < it.endAddr }
        val regionName = region?.name?.ifBlank {
            "${hex64(region.startAddr)} - ${hex64(region.endAddr)}"
        } ?: "${hex64(regionStart)} - ${hex64(regionEnd)}"
        return MemorySearchResult(
            address = address,
            regionName = regionName,
            regionStart = regionStart,
            regionEnd = regionEnd,
            matchSize = matchSize,
            previewBytes = preview.copyOf(),
            previewHex = preview.joinToString(" ") { "%02x".format(it.toInt() and 0xff) },
            valueSummary = describeSearchPreview(valueType, preview, matchSize),
        )
    }

    private fun describeSearchPreview(
        valueType: MemorySearchValueType,
        preview: ByteArray,
        matchSize: Int,
    ): String {
        val slice = preview.copyOf(minOf(matchSize, preview.size))
        return when (valueType) {
            MemorySearchValueType.Int32 ->
                MemoryValueCodec.scalarInt32(slice, slice.size)
                    ?: preview.joinToString(" ") { "%02x".format(it.toInt() and 0xff) }

            MemorySearchValueType.Int64 ->
                MemoryValueCodec.scalarInt64(slice, slice.size)
                    ?: preview.joinToString(" ") { "%02x".format(it.toInt() and 0xff) }

            MemorySearchValueType.Float32 ->
                MemoryValueCodec.scalarFloat32(slice, slice.size)
                    ?: preview.joinToString(" ") { "%02x".format(it.toInt() and 0xff) }

            MemorySearchValueType.Float64 ->
                MemoryValueCodec.scalarFloat64(slice, slice.size)
                    ?: preview.joinToString(" ") { "%02x".format(it.toInt() and 0xff) }

            MemorySearchValueType.HexBytes ->
                preview.joinToString(" ") { "%02x".format(it.toInt() and 0xff) }

            MemorySearchValueType.Ascii ->
                preview.decodeToString()
        }
    }

    private fun hex64(value: ULong): String = "0x${value.toString(16)}"
}
