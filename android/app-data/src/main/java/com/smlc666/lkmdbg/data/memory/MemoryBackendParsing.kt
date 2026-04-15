package com.smlc666.lkmdbg.data.memory

internal fun parseHexBytes(text: String): ByteArray? {
    // Treat any Unicode whitespace as separator.
    val compact = buildString(text.length) {
        for (ch in text) {
            if (!ch.isWhitespace())
                append(ch)
        }
    }
    if (compact.isBlank() || compact.length % 2 != 0)
        return null
    return runCatching {
        ByteArray(compact.length / 2) { index ->
            compact.substring(index * 2, index * 2 + 2).toInt(16).toByte()
        }
    }.getOrNull()
}
