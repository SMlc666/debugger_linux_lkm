package com.smlc666.lkmdbg.data

internal object MemoryValueCodec {
    fun scalarInt32(bytes: ByteArray, remaining: Int): String? =
        scalarInt32Bits(bytes, remaining)?.toString()

    fun scalarInt64(bytes: ByteArray, remaining: Int): String? =
        scalarInt64Bits(bytes, remaining)?.toString()

    fun scalarFloat32(bytes: ByteArray, remaining: Int): String? {
        val bits = scalarInt32Bits(bytes, remaining) ?: return null
        return Float.fromBits(bits).toString()
    }

    fun scalarFloat64(bytes: ByteArray, remaining: Int): String? {
        val bits = scalarInt64Bits(bytes, remaining) ?: return null
        return Double.fromBits(bits).toString()
    }

    fun scalarInt32Bits(bytes: ByteArray, remaining: Int): Int? {
        if (remaining < 4)
            return null
        var value = 0
        repeat(4) { index ->
            value = value or ((bytes[index].toInt() and 0xff) shl (index * 8))
        }
        return value
    }

    fun scalarInt64Bits(bytes: ByteArray, remaining: Int): Long? {
        if (remaining < 8)
            return null
        var value = 0L
        repeat(8) { index ->
            value = value or ((bytes[index].toLong() and 0xffL) shl (index * 8))
        }
        return value
    }
}
