package com.smlc666.lkmdbg.data

import com.smlc666.lkmdbg.shared.BridgeMemorySearchReply
import com.smlc666.lkmdbg.shared.BridgeVmaRecord
import kotlin.math.min

private const val SEARCH_MAX_RESULTS = 512

class MemorySearchEngine(
    private val backend: Backend,
) {
    companion object {
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

    open class PreparedSearchPattern(
        val valueType: MemorySearchValueType,
        val query: String,
        val bytes: ByteArray,
    ) {
        fun describe(preview: ByteArray): String {
            if (preview.size < bytes.size)
                return query
            return describePreview(valueType, preview.copyOf(bytes.size))
        }
    }

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
        val pattern = preparePattern(valueType, query)
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
            results += buildSearchResult(
                vmas = vmas,
                address = record.address,
                regionStart = record.regionStart,
                regionEnd = record.regionEnd,
                preview = preview,
                matchSize = pattern.bytes.size,
                valueType = valueType,
            )
        }

        return MemorySearchOutcome(
            searchedVmaCount = reply.searchedVmaCount.toInt(),
            scannedBytes = reply.scannedBytes,
            results = results,
        )
    }

    suspend fun refine(
        vmas: List<BridgeVmaRecord>,
        sourceResults: List<MemorySearchResult>,
        valueType: MemorySearchValueType,
        refineMode: MemorySearchRefineMode,
        query: String,
        reader: suspend (ULong, UInt) -> ByteArray,
        maxResults: Int = SEARCH_MAX_RESULTS,
    ): MemorySearchOutcome {
        val width = comparisonWidth(valueType, refineMode, query, sourceResults)
        val pattern = if (refineMode == MemorySearchRefineMode.Exact) {
            preparePattern(valueType, query)
        } else {
            null
        }
        val results = ArrayList<MemorySearchResult>(min(sourceResults.size, maxResults))
        var rereadBytes = 0uL

        sourceResults.take(min(sourceResults.size, maxResults)).forEach { result ->
            val preview = reader(result.address, maxOf(width, 16).toUInt())
            rereadBytes += preview.size.toULong()
            if (preview.size < width)
                return@forEach
            if (!matchesRefineMode(
                    mode = refineMode,
                    valueType = valueType,
                    previous = result.previewBytes,
                    current = preview,
                    width = width,
                    exactPattern = pattern,
                )
            ) {
                return@forEach
            }
            results += buildSearchResult(
                vmas = vmas,
                address = result.address,
                regionStart = result.regionStart,
                regionEnd = result.regionEnd,
                preview = preview,
                matchSize = width,
                valueType = valueType,
            )
        }

        return MemorySearchOutcome(
            searchedVmaCount = sourceResults.size,
            scannedBytes = rereadBytes,
            results = results,
        )
    }

    fun preparePattern(
        valueType: MemorySearchValueType,
        query: String,
    ): PreparedSearchPattern = SearchPattern.parse(valueType, query)

    private fun describeVma(it: BridgeVmaRecord): String = describeRange(it.startAddr, it.endAddr)

    private fun describeRange(start: ULong, end: ULong): String =
        buildString {
            append(hex64(start))
            append(" - ")
            append(hex64(end))
        }

    private fun hex64(value: ULong): String = "0x${value.toString(16)}"

    private fun buildSearchResult(
        vmas: List<BridgeVmaRecord>,
        address: ULong,
        regionStart: ULong,
        regionEnd: ULong,
        preview: ByteArray,
        matchSize: Int,
        valueType: MemorySearchValueType,
    ): MemorySearchResult {
        val region = vmas.firstOrNull {
            address >= it.startAddr && address < it.endAddr
        }
        val regionName = if (region != null) {
            region.name.ifBlank { describeVma(region) }
        } else {
            describeRange(regionStart, regionEnd)
        }
        val effectiveSize = min(matchSize, preview.size)
        val previewSlice = preview.copyOf(effectiveSize)
        return MemorySearchResult(
            address = address,
            regionName = regionName,
            regionStart = regionStart,
            regionEnd = regionEnd,
            matchSize = matchSize,
            previewBytes = preview.copyOf(),
            previewHex = preview.joinToString(" ") { "%02x".format(it.toInt() and 0xff) },
            valueSummary = describePreview(valueType, previewSlice),
        )
    }

    private fun comparisonWidth(
        valueType: MemorySearchValueType,
        refineMode: MemorySearchRefineMode,
        query: String,
        sourceResults: List<MemorySearchResult>,
    ): Int {
        if (refineMode == MemorySearchRefineMode.Exact)
            return preparePattern(valueType, query).bytes.size

        return when (valueType) {
            MemorySearchValueType.Int32, MemorySearchValueType.Float32 -> 4
            MemorySearchValueType.Int64, MemorySearchValueType.Float64 -> 8
            MemorySearchValueType.HexBytes, MemorySearchValueType.Ascii -> {
                if (query.isNotBlank()) {
                    preparePattern(valueType, query).bytes.size
                } else {
                    sourceResults.firstOrNull()?.matchSize
                        ?: throw IllegalArgumentException("run an exact search first for this refine mode")
                }
            }
        }
    }

    private fun matchesRefineMode(
        mode: MemorySearchRefineMode,
        valueType: MemorySearchValueType,
        previous: ByteArray,
        current: ByteArray,
        width: Int,
        exactPattern: PreparedSearchPattern?,
    ): Boolean {
        if (previous.size < width || current.size < width)
            return false

        val previousSlice = previous.copyOf(width)
        val currentSlice = current.copyOf(width)
        return when (mode) {
            MemorySearchRefineMode.Exact -> {
                val pattern = requireNotNull(exactPattern)
                currentSlice.contentEquals(pattern.bytes)
            }

            MemorySearchRefineMode.Changed -> !currentSlice.contentEquals(previousSlice)
            MemorySearchRefineMode.Unchanged -> currentSlice.contentEquals(previousSlice)
            MemorySearchRefineMode.Increased ->
                compareNumeric(valueType, currentSlice, previousSlice) > 0

            MemorySearchRefineMode.Decreased ->
                compareNumeric(valueType, currentSlice, previousSlice) < 0
        }
    }

    private fun compareNumeric(
        valueType: MemorySearchValueType,
        current: ByteArray,
        previous: ByteArray,
    ): Int = when (valueType) {
        MemorySearchValueType.Int32 -> leInt(current).compareTo(leInt(previous))
        MemorySearchValueType.Int64 -> leLong(current).compareTo(leLong(previous))
        MemorySearchValueType.Float32 -> leFloat(current).compareTo(leFloat(previous))
        MemorySearchValueType.Float64 -> leDouble(current).compareTo(leDouble(previous))
        MemorySearchValueType.HexBytes, MemorySearchValueType.Ascii ->
            throw IllegalArgumentException("increased/decreased only supported for numeric refine")
    }

    private fun describePreview(
        valueType: MemorySearchValueType,
        preview: ByteArray,
    ): String {
        if (preview.isEmpty())
            return ""
        return when (valueType) {
            MemorySearchValueType.Int32 ->
                if (preview.size >= 4) leInt(preview).toString() else preview.joinToString(" ") { "%02x".format(it.toInt() and 0xff) }

            MemorySearchValueType.Int64 ->
                if (preview.size >= 8) leLong(preview).toString() else preview.joinToString(" ") { "%02x".format(it.toInt() and 0xff) }

            MemorySearchValueType.Float32 ->
                if (preview.size >= 4) leFloat(preview).toString() else preview.joinToString(" ") { "%02x".format(it.toInt() and 0xff) }

            MemorySearchValueType.Float64 ->
                if (preview.size >= 8) leDouble(preview).toString() else preview.joinToString(" ") { "%02x".format(it.toInt() and 0xff) }

            MemorySearchValueType.HexBytes ->
                preview.joinToString(" ") { "%02x".format(it.toInt() and 0xff) }

            MemorySearchValueType.Ascii ->
                preview.decodeToString()
        }
    }

    private class SearchPattern(
        valueType: MemorySearchValueType,
        query: String,
        bytes: ByteArray,
    ) : PreparedSearchPattern(
        valueType = valueType,
        query = query,
        bytes = bytes,
    ) {
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
        }
    }
}
