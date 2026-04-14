package com.smlc666.lkmdbg.data

object NativeDisassembler {
    init {
        System.loadLibrary("lkmdbg_native")
    }

    fun disassembleArm64(
        baseAddress: ULong,
        bytes: ByteArray,
        maxInstructions: Int = 24,
    ): List<String> {
        if (bytes.isEmpty())
            return emptyList()
        return nativeDisassembleArm64(baseAddress.toLong(), bytes, maxInstructions).toList()
    }

    private external fun nativeDisassembleArm64(
        baseAddress: Long,
        bytes: ByteArray,
        maxInstructions: Int,
    ): Array<String>
}
