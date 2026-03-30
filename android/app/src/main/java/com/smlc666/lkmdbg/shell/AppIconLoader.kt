package com.smlc666.lkmdbg.shell

import android.content.Context
import android.graphics.drawable.Drawable

class AppIconLoader(
    context: Context,
) {
    private val packageManager = context.applicationContext.packageManager
    private val cache = mutableMapOf<String, Drawable?>()

    fun load(packageName: String?): Drawable? {
        if (packageName.isNullOrBlank())
            return null
        return cache.getOrPut(packageName) {
            runCatching { packageManager.getApplicationIcon(packageName) }.getOrNull()
        }
    }
}
