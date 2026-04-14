package com.smlc666.lkmdbg.data.memory

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class MemoryBackendParsingTest {
    @Test
    fun parseHexBytes_acceptsSpaces_andRejectsOddLength() {
        val parsed = parseHexBytes("01 02 ff")
        assertEquals(listOf(0x01, 0x02, 0xff), parsed!!.map { it.toInt() and 0xff })
        assertNull(parseHexBytes("0"))
    }

    @Test
    fun parseHexBytes_acceptsMixedWhitespace_andRejectsNonHex() {
        val parsed = parseHexBytes("01\n02\tff")
        assertEquals(listOf(0x01, 0x02, 0xff), parsed!!.map { it.toInt() and 0xff })
        assertNull(parseHexBytes("zz"))
    }
}
