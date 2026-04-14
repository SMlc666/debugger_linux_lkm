package com.smlc666.lkmdbg.data

object NativeAssembler {
    init {
        System.loadLibrary("lkmdbg_native")
    }

    fun assembleArm64(
        baseAddress: ULong,
        source: String,
    ): ByteArray {
        require(source.isNotBlank()) { "assembly source must not be empty" }
        return nativeAssembleArm64(baseAddress.toLong(), source)
    }

    private external fun nativeAssembleArm64(
        baseAddress: Long,
        source: String,
    ): ByteArray
}
