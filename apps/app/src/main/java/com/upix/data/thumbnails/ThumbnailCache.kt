package com.upix.data.thumbnails

import android.util.LruCache
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asAndroidBitmap
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit

private const val THUMBNAIL_DECODE_PARALLELISM = 2

internal object ThumbnailMemoryCache {
    // 缓存按字节计价（KB）：以堆上限 1/8 为预算，避免大图条目挤爆小图。
    private const val BYTES_PER_KB = 1024
    private val THUMBNAIL_CACHE_KB =
        (Runtime.getRuntime().maxMemory() / 8 / BYTES_PER_KB).toInt()
    private const val SOURCE_CANDIDATE_CACHE_SIZE = 128
    private val thumbnailCache = object : LruCache<String, ImageBitmap>(THUMBNAIL_CACHE_KB) {
        override fun sizeOf(key: String, value: ImageBitmap): Int =
            value.asAndroidBitmap().byteCount / BYTES_PER_KB
    }
    private val sourceCandidateCache = LruCache<String, List<String>>(SOURCE_CANDIDATE_CACHE_SIZE)

    fun getThumbnail(sourceKey: String): ImageBitmap? {
        if (sourceKey.isBlank()) {
            return null
        }
        return synchronized(this) {
            thumbnailCache.get(sourceKey)
        }
    }

    fun putThumbnail(sourceKey: String, bitmap: ImageBitmap) {
        if (sourceKey.isBlank()) {
            return
        }
        synchronized(this) {
            thumbnailCache.put(sourceKey, bitmap)
        }
    }

    fun getSourceCandidates(sourceKey: String): List<String>? {
        if (sourceKey.isBlank()) {
            return null
        }
        return synchronized(this) {
            sourceCandidateCache.get(sourceKey)
        }
    }

    fun putSourceCandidates(sourceKey: String, candidates: List<String>) {
        if (sourceKey.isBlank() || candidates.isEmpty()) {
            return
        }
        synchronized(this) {
            sourceCandidateCache.put(sourceKey, candidates)
        }
    }
}

internal object ThumbnailDecodeLimiter {
    private val semaphore = Semaphore(THUMBNAIL_DECODE_PARALLELISM)

    suspend fun <T> withPermit(block: () -> T): T {
        return semaphore.withPermit {
            block()
        }
    }
}
