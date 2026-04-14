package com.smlc666.lkmdbg.data.memory

internal fun parseHexBytes(text: String): ByteArray? {
    val compact = text.filterNot { it.isWhitespace() }
    if (compact.isBlank() || compact.length % 2 != 0)
        return null
    return runCatching {
        ByteArray(compact.length / 2) { index ->
            compact.substring(index * 2, index * 2 + 2).toInt(16).toByte()
        }
    }.getOrNull()
}
