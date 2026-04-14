package com.smlc666.lkmdbg.data

import android.content.Context
import com.smlc666.lkmdbg.appdata.R
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.update

internal class MemoryEditorController(
    context: Context,
    private val client: PipeAgentClient,
    private val stateFlow: MutableStateFlow<SessionBridgeState>,
    private val memoryPreviewBuilder: MemoryPreviewBuilder,
    private val pageSize: UInt,
) {
    private val appContext = context.applicationContext

    suspend fun openPage(remoteAddr: ULong) {
        val pageStart = memoryPreviewBuilder.alignDown(remoteAddr)
        val reply = client.readMemory(pageStart, pageSize)
        ensureBridgeStatusOk(reply.status, reply.message, "READ_MEMORY")
        val bytes = reply.data.copyOf(reply.bytesDone.toInt())
        stateFlow.update { current ->
            current.copy(
                memoryAddressInput = hex64(remoteAddr),
                memoryPage = memoryPreviewBuilder.buildPage(remoteAddr, pageStart, bytes, current.vmas),
                lastMessage = reply.message.ifBlank {
                    appContext.getString(
                        R.string.memory_message_preview,
                        reply.bytesDone.toString(),
                        hex64(pageStart),
                    )
                },
            )
        }
    }

    suspend fun selectAddress(remoteAddr: ULong) {
        val current = stateFlow.value.memoryPage
        if (current != null && memoryPreviewBuilder.addressInsidePage(current, remoteAddr)) {
            stateFlow.update { snapshot ->
                val page = snapshot.memoryPage ?: return@update snapshot
                snapshot.copy(
                    memoryAddressInput = hex64(remoteAddr),
                    memoryPage = memoryPreviewBuilder.retargetPage(page, remoteAddr),
                    lastMessage = appContext.getString(R.string.memory_message_cursor, hex64(remoteAddr)),
                )
            }
            return
        }
        openPage(remoteAddr)
    }

    fun parseAddressInput(value: String): ULong? {
        val trimmed = value.trim()
        if (trimmed.isEmpty())
            return null
        if (trimmed.startsWith("0x", ignoreCase = true))
            return trimmed.removePrefix("0x").removePrefix("0X").toULongOrNull(16)
        return trimmed.toULongOrNull() ?: trimmed.toULongOrNull(16)
    }

    fun parseHexBytes(value: String): ByteArray? {
        val compact = value.replace(" ", "").replace("\n", "").replace("\t", "")
        if (compact.isEmpty() || compact.length % 2 != 0)
            return null
        return runCatching {
            ByteArray(compact.length / 2) { index ->
                compact.substring(index * 2, index * 2 + 2).toInt(16).toByte()
            }
        }.getOrNull()
    }

    fun currentSelectionBytes(): ByteArray? {
        val page = stateFlow.value.memoryPage ?: return null
        return memoryPreviewBuilder.selectionBytes(page, stateFlow.value.memorySelectionSize)
    }

    suspend fun loadSelectionIntoHexSearch() {
        val bytes = currentSelectionBytes()
            ?: throw IllegalStateException(appContext.getString(R.string.memory_error_no_selection))
        val query = bytes.joinToString(" ") { "%02x".format(it.toInt() and 0xff) }
        stateFlow.update { current ->
            current.copy(
                memorySearch = current.memorySearch.copy(
                    valueType = MemorySearchValueType.HexBytes,
                    query = query,
                ),
                lastMessage = appContext.getString(R.string.memory_message_search_query_loaded, query),
            )
        }
    }

    suspend fun loadSelectionIntoAsciiSearch() {
        val bytes = currentSelectionBytes()
            ?: throw IllegalStateException(appContext.getString(R.string.memory_error_no_selection))
        val query = bytes.decodeToString()
        stateFlow.update { current ->
            current.copy(
                memorySearch = current.memorySearch.copy(
                    valueType = MemorySearchValueType.Ascii,
                    query = query,
                ),
                lastMessage = appContext.getString(R.string.memory_message_search_query_loaded, query),
            )
        }
    }

    suspend fun loadSelectionIntoEditors() {
        val bytes = currentSelectionBytes()
            ?: throw IllegalStateException(appContext.getString(R.string.memory_error_no_selection))
        stateFlow.update { current ->
            current.copy(
                memoryWriteHexInput = bytes.joinToString(" ") { "%02x".format(it.toInt() and 0xff) },
                memoryWriteAsciiInput = bytes.decodeToString(),
                lastMessage = appContext.getString(R.string.memory_message_editor_loaded, bytes.size),
            )
        }
    }

    suspend fun assembleArm64ToEditors() {
        val page = stateFlow.value.memoryPage
            ?: throw IllegalStateException(appContext.getString(R.string.memory_error_no_page))
        val source = stateFlow.value.memoryWriteAsmInput.trim()
        if (source.isEmpty())
            throw IllegalStateException(appContext.getString(R.string.memory_error_invalid_asm))
        val encoded = NativeAssembler.assembleArm64(page.focusAddress, source)
        if (encoded.isEmpty())
            throw IllegalStateException(appContext.getString(R.string.memory_error_invalid_asm))
        stateFlow.update { current ->
            current.copy(
                memoryWriteHexInput = encoded.joinToString(" ") { "%02x".format(it.toInt() and 0xff) },
                memoryWriteAsciiInput = encoded.decodeToString(),
                lastMessage = appContext.getString(
                    R.string.memory_message_asm_complete,
                    encoded.size,
                    hex64(page.focusAddress),
                ),
            )
        }
    }

    suspend fun assembleArm64AndWrite() {
        val page = stateFlow.value.memoryPage
            ?: throw IllegalStateException(appContext.getString(R.string.memory_error_no_page))
        val source = stateFlow.value.memoryWriteAsmInput.trim()
        if (source.isEmpty())
            throw IllegalStateException(appContext.getString(R.string.memory_error_invalid_asm))
        val encoded = NativeAssembler.assembleArm64(page.focusAddress, source)
        if (encoded.isEmpty())
            throw IllegalStateException(appContext.getString(R.string.memory_error_invalid_asm))
        val reply = client.writeMemory(page.focusAddress, encoded)
        if (reply.status != 0 || reply.bytesDone.toInt() != encoded.size)
            throw IllegalStateException(reply.message.ifBlank { "write failed bytes_done=${reply.bytesDone}" })
        openPage(page.focusAddress)
        stateFlow.update { current ->
            current.copy(
                memoryWriteHexInput = encoded.joinToString(" ") { "%02x".format(it.toInt() and 0xff) },
                memoryWriteAsciiInput = encoded.decodeToString(),
                lastMessage = appContext.getString(
                    R.string.memory_message_asm_write_complete,
                    encoded.size,
                    hex64(page.focusAddress),
                ),
            )
        }
    }

    suspend fun writeHexAtFocus() {
        val page = stateFlow.value.memoryPage
            ?: throw IllegalStateException(appContext.getString(R.string.memory_error_no_page))
        val data = parseHexBytes(stateFlow.value.memoryWriteHexInput)
        if (data == null || data.isEmpty())
            throw IllegalStateException(appContext.getString(R.string.memory_error_invalid_hex))
        writeBytesAtFocus(page.focusAddress, data)
    }

    suspend fun writeAsciiAtFocus() {
        val page = stateFlow.value.memoryPage
            ?: throw IllegalStateException(appContext.getString(R.string.memory_error_no_page))
        val data = stateFlow.value.memoryWriteAsciiInput.toByteArray(Charsets.UTF_8)
        if (data.isEmpty())
            throw IllegalStateException(appContext.getString(R.string.memory_error_invalid_ascii))
        writeBytesAtFocus(page.focusAddress, data)
    }

    private suspend fun writeBytesAtFocus(focusAddress: ULong, data: ByteArray) {
        val reply = client.writeMemory(focusAddress, data)
        if (reply.status != 0 || reply.bytesDone.toInt() != data.size)
            throw IllegalStateException(reply.message.ifBlank { "write failed bytes_done=${reply.bytesDone}" })
        openPage(focusAddress)
        stateFlow.update { current ->
            current.copy(
                memoryWriteHexInput = data.joinToString(" ") { "%02x".format(it.toInt() and 0xff) },
                memoryWriteAsciiInput = data.decodeToString(),
                lastMessage = reply.message.ifBlank {
                    appContext.getString(
                        R.string.memory_message_write_complete,
                        data.size,
                        hex64(focusAddress),
                    )
                },
            )
        }
    }
}
