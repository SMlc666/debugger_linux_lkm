package com.smlc666.lkmdbg.data

internal fun ensureBridgeStatusOk(status: Int, message: String, command: String) {
    if (status < 0)
        throw IllegalStateException(message.ifBlank { "$command failed with status=$status" })
}

internal fun hex64(value: ULong): String = "0x${value.toString(16)}"
