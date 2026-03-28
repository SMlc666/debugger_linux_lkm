package com.smlc666.lkmdbg.data

import kotlin.math.min

private const val SEARCH_CHUNK_BYTES = 64 * 1024
private const val SEARCH_MAX_RESULTS = 128

class MemorySearchEngine(
    private val backend: Backend,
) {
    interface Backend {
        suspend fun read(remoteAddr: ULong, length: UInt): ByteArray
    }

    suspend fun search(
        vmas: List<com.smlc666.lkmdbg.shared.BridgeVmaRecord>,
        preset: MemoryRegionPreset,
        valueType: MemorySearchValueType,
        query: String,
        maxResults: Int = SEARCH_MAX_RESULTS,
    ): MemorySearchOutcome {
        val pattern = SearchPattern.parse(valueType, query)
        val results = ArrayList<MemorySearchResult>(min(maxResults, SEARCH_MAX_RESULTS))
        var searchedVmas = 0
        var scannedBytes = 0UL

        for (vma in vmas) {
            if (!preset.matches(vma))
                continue
            searchedVmas += 1
            scannedBytes += searchVma(vma, pattern, results, maxResults)
            if (results.size >= maxResults)
                break
        }

        return MemorySearchOutcome(
            searchedVmaCount = searchedVmas,
            scannedBytes = scannedBytes,
            results = results,
        )
    }

    private suspend fun searchVma(
        vma: com.smlc666.lkmdbg.shared.BridgeVmaRecord,
        pattern: SearchPattern,
        results: MutableList<MemorySearchResult>,
        maxResults: Int,
    ): ULong {
        val overlap = (pattern.bytes.size - 1).coerceAtLeast(0)
        var carry = ByteArray(0)
        var cursor = vma.startAddr
        var scanned = 0UL

        while (cursor < vma.endAddr && results.size < maxResults) {
            val remaining = (vma.endAddr - cursor).toLong()
            val requestSize = min(SEARCH_CHUNK_BYTES.toLong(), remaining).toInt()
            if (requestSize <= 0)
                break

            val chunk = backend.read(cursor, requestSize.toUInt())
            if (chunk.isEmpty())
                break
            scanned += chunk.size.toULong()

            val combined = ByteArray(carry.size + chunk.size)
            if (carry.isNotEmpty())
                carry.copyInto(combined, endIndex = carry.size)
            chunk.copyInto(combined, destinationOffset = carry.size)

            var index = 0
            while (index <= combined.size - pattern.bytes.size && results.size < maxResults) {
                if (combined.regionMatches(index, pattern.bytes)) {
                    if (index + pattern.bytes.size > carry.size || carry.isEmpty()) {
                        val absolute = cursor - carry.size.toUInt().toULong() + index.toUInt().toULong()
                        val preview = combined.copyOfRange(
                            index,
                            min(combined.size, index + maxOf(pattern.bytes.size, 16)),
                        )
                        results.add(
                            MemorySearchResult(
                            address = absolute,
                            regionName = vma.name.ifBlank { describeVma(vma) },
                            regionStart = vma.startAddr,
                            regionEnd = vma.endAddr,
                            previewHex = preview.joinToString(" ") { "%02x".format(it.toInt() and 0xff) },
                            valueSummary = pattern.describe(preview),
                            ),
                        )
                    }
                    index += pattern.bytes.size.coerceAtLeast(1)
                } else {
                    index += 1
                }
            }

            carry = if (overlap == 0) {
                ByteArray(0)
            } else {
                combined.copyOfRange(maxOf(0, combined.size - overlap), combined.size)
            }
            cursor += chunk.size.toUInt().toULong()
        }

        return scanned
    }

    private fun ByteArray.regionMatches(offset: Int, other: ByteArray): Boolean {
        if (offset < 0 || offset + other.size > size)
            return false
        for (i in other.indices) {
            if (this[offset + i] != other[i])
                return false
        }
        return true
    }

    private fun describeVma(vma: com.smlc666.lkmdbg.shared.BridgeVmaRecord): String =
        buildString {
            append(hex64(vma.startAddr))
            append(" - ")
            append(hex64(vma.endAddr))
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
