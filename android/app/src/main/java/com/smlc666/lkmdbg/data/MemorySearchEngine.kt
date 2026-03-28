package com.smlc666.lkmdbg.data

import com.smlc666.lkmdbg.shared.BridgeMemorySearchReply
import com.smlc666.lkmdbg.shared.BridgeVmaRecord
import kotlin.math.min

private const val SEARCH_MAX_RESULTS = 128

class MemorySearchEngine(
    private val backend: Backend,
) {
    interface Backend {
        suspend fun search(regionPreset: UInt, maxResults: UInt, pattern: ByteArray): BridgeMemorySearchReply
    }

    suspend fun search(
        vmas: List<BridgeVmaRecord>,
        preset: MemoryRegionPreset,
        valueType: MemorySearchValueType,
        query: String,
        maxResults: Int = SEARCH_MAX_RESULTS,
    ): MemorySearchOutcome {
        val pattern = SearchPattern.parse(valueType, query)
        val reply = backend.search(
            regionPreset = preset.wireValue,
            maxResults = min(maxResults, SEARCH_MAX_RESULTS).toUInt(),
            pattern = pattern.bytes,
        )
        require(reply.status == 0) { reply.message.ifBlank { "search failed status=${reply.status}" } }

        val results = ArrayList<MemorySearchResult>(reply.results.size)
        for (record in reply.results) {
            val previewSize = min(record.previewSize.toInt(), record.preview.size)
            val preview = record.preview.copyOf(previewSize)
            val region = vmas.firstOrNull {
                record.address >= it.startAddr && record.address < it.endAddr
            }
            val regionName = if (region != null) {
                region.name.ifBlank { describeVma(region) }
            } else {
                describeRange(record.regionStart, record.regionEnd)
            }
            results += MemorySearchResult(
                address = record.address,
                regionName = regionName,
                regionStart = record.regionStart,
                regionEnd = record.regionEnd,
                previewHex = preview.joinToString(" ") { "%02x".format(it.toInt() and 0xff) },
                valueSummary = pattern.describe(preview),
            )
        }

        return MemorySearchOutcome(
            searchedVmaCount = reply.searchedVmaCount.toInt(),
            scannedBytes = reply.scannedBytes,
            results = results,
        )
    }

    private fun describeVma(it: BridgeVmaRecord): String = describeRange(it.startAddr, it.endAddr)

    private fun describeRange(start: ULong, end: ULong): String =
        buildString {
            append(hex64(start))
            append(" - ")
            append(hex64(end))
        }

    private fun hex64(value: ULong): String = "0x${value.toString(16)}"

    private data class SearchPattern(
        val valueType: MemorySearchValueType,
        val query: String,
        val bytes: ByteArray,
    ) {
        fun describe(preview: ByteArray): String {
            if (preview.size < bytes.size)
                return query
            return when (valueType) {
                MemorySearchValueType.Int32 -> leInt(preview).toString()
                MemorySearchValueType.Int64 -> leLong(preview).toString()
                MemorySearchValueType.Float32 -> leFloat(preview).toString()
                MemorySearchValueType.Float64 -> leDouble(preview).toString()
                MemorySearchValueType.HexBytes -> bytes.joinToString(" ") { "%02x".format(it.toInt() and 0xff) }
                MemorySearchValueType.Ascii -> preview.copyOf(bytes.size).decodeToString()
            }
        }

        companion object {
            fun parse(type: MemorySearchValueType, query: String): SearchPattern {
                val trimmed = query.trim()
                require(trimmed.isNotEmpty()) { "empty search query" }
                val bytes = when (type) {
                    MemorySearchValueType.Int32 -> {
                        val value = trimmed.toLong().toInt()
                        byteArrayOf(
                            value.toByte(),
                            (value ushr 8).toByte(),
                            (value ushr 16).toByte(),
                            (value ushr 24).toByte(),
                        )
                    }
                    MemorySearchValueType.Int64 -> {
                        val value = trimmed.toLong()
                        ByteArray(Long.SIZE_BYTES) { shift ->
                            (value ushr (shift * 8)).toByte()
                        }
                    }
                    MemorySearchValueType.Float32 -> {
                        val bits = trimmed.toFloat().toBits()
                        byteArrayOf(
                            bits.toByte(),
                            (bits ushr 8).toByte(),
                            (bits ushr 16).toByte(),
                            (bits ushr 24).toByte(),
                        )
                    }
                    MemorySearchValueType.Float64 -> {
                        val bits = trimmed.toDouble().toBits()
                        ByteArray(Long.SIZE_BYTES) { shift ->
                            (bits ushr (shift * 8)).toByte()
                        }
                    }
                    MemorySearchValueType.HexBytes -> parseHexBytes(trimmed)
                    MemorySearchValueType.Ascii -> trimmed.toByteArray(Charsets.UTF_8)
                }
                return SearchPattern(type, trimmed, bytes)
            }

            private fun parseHexBytes(text: String): ByteArray {
                val compact = text.replace(" ", "").replace("\n", "")
                require(compact.length % 2 == 0) { "hex bytes must have even length" }
                return ByteArray(compact.length / 2) { index ->
                    compact.substring(index * 2, index * 2 + 2).toInt(16).toByte()
                }
            }

            private fun leInt(bytes: ByteArray): Int =
                (bytes[0].toInt() and 0xff) or
                    ((bytes[1].toInt() and 0xff) shl 8) or
                    ((bytes[2].toInt() and 0xff) shl 16) or
                    ((bytes[3].toInt() and 0xff) shl 24)

            private fun leLong(bytes: ByteArray): Long {
                var value = 0L
                for (i in 0 until Long.SIZE_BYTES)
                    value = value or ((bytes[i].toLong() and 0xffL) shl (i * 8))
                return value
            }

            private fun leFloat(bytes: ByteArray): Float = Float.fromBits(leInt(bytes))

            private fun leDouble(bytes: ByteArray): Double = Double.fromBits(leLong(bytes))
        }
    }
}
