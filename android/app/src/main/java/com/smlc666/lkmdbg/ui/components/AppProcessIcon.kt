package com.smlc666.lkmdbg.ui.components

import android.graphics.Bitmap
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.graphics.drawable.toBitmap
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

private object AppIconBitmapCache {
    private val cache = HashMap<String, Bitmap?>()

    @Synchronized
    fun get(key: String): Bitmap? = cache[key]

    @Synchronized
    fun put(key: String, value: Bitmap?) {
        cache[key] = value
    }
}

@Composable
internal fun AppProcessIcon(
    packageName: String?,
    displayName: String,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current
    val cacheKey = packageName ?: "fallback:${displayName.lowercase()}"
    val bitmap by produceState<Bitmap?>(initialValue = AppIconBitmapCache.get(cacheKey), cacheKey) {
        if (value != null || packageName == null)
            return@produceState
        value = withContext(Dispatchers.IO) {
            runCatching {
                @Suppress("DEPRECATION")
                context.packageManager.getApplicationIcon(packageName).toBitmap(112, 112)
            }.getOrNull().also { AppIconBitmapCache.put(cacheKey, it) }
        }
    }

    val loadedBitmap = bitmap
    if (loadedBitmap != null) {
        Image(
            bitmap = loadedBitmap.asImageBitmap(),
            contentDescription = displayName,
            modifier = modifier
                .size(52.dp)
                .clip(RoundedCornerShape(16.dp)),
        )
        return
    }

    Box(
        modifier = modifier
            .size(52.dp)
            .clip(RoundedCornerShape(16.dp))
            .background(MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.78f)),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text = displayName.take(2).uppercase(),
            style = MaterialTheme.typography.titleMedium,
            color = MaterialTheme.colorScheme.onPrimaryContainer,
        )
    }
}
