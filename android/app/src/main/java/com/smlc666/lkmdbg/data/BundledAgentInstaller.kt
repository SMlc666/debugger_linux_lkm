package com.smlc666.lkmdbg.data

import android.content.Context
import java.io.File

private const val AGENT_ASSET_PATH = "agent/arm64-v8a/lkmdbg-agent"

object BundledAgentInstaller {
    fun installedAgentPath(context: Context): String =
        File(File(context.filesDir, "agent-bin"), "lkmdbg-agent").absolutePath

    fun install(context: Context): String {
        val outFile = File(File(context.filesDir, "agent-bin"), "lkmdbg-agent")
        outFile.parentFile?.mkdirs()

        context.assets.open(AGENT_ASSET_PATH).use { input ->
            outFile.outputStream().use { output ->
                input.copyTo(output)
            }
        }

        outFile.setReadable(true, true)
        outFile.setWritable(true, true)
        outFile.setExecutable(true, true)
        return outFile.absolutePath
    }
}
