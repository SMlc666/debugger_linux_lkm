package com.smlc666.lkmdbg.nativeui

import android.content.Context
import java.io.File
import java.util.Locale

internal object NativeFontCatalog {
    private val bundledFontAssets = listOf(
        "NotoSansSC-Regular.otf",
    )

    private val cjkPreferredNameTokens = listOf(
        "notosanssc",
        "notosanscjk",
        "notosanscjksc",
        "sourcehansans",
        "sourcehansanscn",
        "droidsansfallback",
        "fallback",
        "pingfang",
        "harmonyossanssc",
        "lantinghei",
        "misans",
    )

    private val secondaryNameTokens = listOf(
        "notosans",
        "sourcesans",
        "sans",
    )

    private fun scoreFont(file: File): Int {
        val name = file.name.lowercase(Locale.ROOT)
        return when {
            cjkPreferredNameTokens.any(name::contains) && name.endsWith(".ttf") -> 0
            cjkPreferredNameTokens.any(name::contains) && name.endsWith(".otf") -> 1
            cjkPreferredNameTokens.any(name::contains) -> 2
            secondaryNameTokens.any(name::contains) && name.endsWith(".ttf") -> 3
            secondaryNameTokens.any(name::contains) && name.endsWith(".otf") -> 4
            secondaryNameTokens.any(name::contains) -> 5
            name.endsWith(".ttf") -> 6
            name.endsWith(".otf") -> 7
            else -> 8
        }
    }

    fun buildCandidatePaths(context: Context): Array<String> {
        installBundledFonts(context)
        val candidates = mutableListOf<File>()
        val dirs = listOf(
            File(context.filesDir, "fonts"),
            File("/sdcard/lkmdbg/fonts"),
            File("/sdcard/Android/data/${context.packageName}/files/fonts"),
            File("/system/fonts"),
            File("/product/fonts"),
            File("/system_ext/fonts"),
            File("/vendor/fonts"),
        )

        dirs.forEach { dir ->
            if (!dir.isDirectory)
                return@forEach
            dir.walkTopDown()
                .maxDepth(2)
                .filter { it.isFile }
                .filter { file ->
                    val name = file.name.lowercase(Locale.ROOT)
                    name.endsWith(".ttf") || name.endsWith(".otf") || name.endsWith(".ttc")
                }
                .forEach(candidates::add)
        }

        return candidates
            .distinctBy { it.absolutePath }
            .sortedWith(
                compareBy<File> { file -> scoreFont(file) }
                    .thenBy { it.name.lowercase(Locale.ROOT) },
            )
            .map { file -> file.absolutePath }
            .toTypedArray()
    }

    fun installBundledFonts(context: Context) {
        val targetDir = File(context.filesDir, "fonts")
        if (!targetDir.exists())
            targetDir.mkdirs()

        for (assetName in bundledFontAssets) {
            val target = File(targetDir, assetName)
            if (target.isFile && target.length() > 0L)
                continue
            runCatching {
                context.assets.open("fonts/$assetName").use { input ->
                    target.outputStream().use { output ->
                        input.copyTo(output)
                    }
                }
            }
        }
    }
}
