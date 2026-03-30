package com.smlc666.lkmdbg.nativeui

import android.content.Context
import java.io.File
import java.util.Locale

internal object NativeFontCatalog {
    private val preferredNameTokens = listOf(
        "notosanssc",
        "notosanscjk",
        "sourcehansans",
        "sourcesans",
        "droidsansfallback",
        "misans",
        "sans",
    )

    fun buildCandidatePaths(context: Context): Array<String> {
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
                compareByDescending<File> { file ->
                    val name = file.name.lowercase(Locale.ROOT)
                    preferredNameTokens.any(name::contains)
                }.thenBy<File> { file ->
                    when {
                        file.name.endsWith(".ttf", ignoreCase = true) -> 0
                        file.name.endsWith(".otf", ignoreCase = true) -> 1
                        else -> 2
                    }
                }.thenBy { it.name.lowercase(Locale.ROOT) },
            )
            .map(File::absolutePath)
            .toTypedArray()
    }
}
