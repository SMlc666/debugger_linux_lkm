package com.smlc666.lkmdbg.overlay.ui.memory

data class MemoryDataRow(
    val address: ULong,
    val title: String,
    val subtitle: String? = null,
    val value: String? = null,
) {
    val addressHex: String
        get() = "0x${address.toString(16)}"
}
