package io.upix.util

import android.content.Context
import android.content.Intent
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.Build
import androidx.core.content.FileProvider
import io.upix.data.repository.LogRepository
import java.io.File

/**
 * URI / intent 相关的纯工具（无 Compose 依赖），从 UpixApp.kt 原样迁出：
 * 输出图片的 ACTION_VIEW 分发（含 FileProvider 映射）、SAF 持久化授权、
 * 损坏图片源的边界解码探测、旧版存储权限判定。
 */

internal fun openImageOutput(
    context: Context,
    rawOutput: String,
    logRepository: LogRepository
): Boolean {
    val normalizedOutput = rawOutput.trim()
    if (normalizedOutput.isBlank()) {
        return false
    }

    val viewUri = resolveOutputViewUri(context = context, rawOutput = normalizedOutput)
    if (viewUri == null) {
        logRepository.warning("UpixApp", "Cannot resolve output URI: $normalizedOutput")
        return false
    }

    val viewIntent = Intent(Intent.ACTION_VIEW).apply {
        setDataAndType(viewUri, "image/*")
        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
    }
    val canHandle = viewIntent.resolveActivity(context.packageManager) != null
    if (!canHandle) {
        logRepository.warning("UpixApp", "No app can open output image: $viewUri")
        return false
    }

    return runCatching {
        context.startActivity(viewIntent)
        true
    }.onFailure { throwable ->
        logRepository.warning("UpixApp", "Failed to open output image: ${throwable.message}")
    }.getOrDefault(false)
}

private fun resolveOutputViewUri(
    context: Context,
    rawOutput: String
): Uri? {
    if (rawOutput.startsWith("/")) {
        return fileViewUri(context, File(rawOutput))
    }

    val parsedUri = runCatching { Uri.parse(rawOutput) }.getOrNull() ?: return null
    return when (parsedUri.scheme?.lowercase()) {
        null -> fileViewUri(context, File(rawOutput))
        "content" -> parsedUri
        "file" -> fileViewUri(context, File(parsedUri.path.orEmpty()))
        else -> null
    }
}

/**
 * 文件路径 → FileProvider content URI。路径不在 file_paths.xml 任何已配置
 * root 下时 getUriForFile 会抛 IllegalArgumentException——历史队列记录里可能
 * 存有现在未映射的输出路径，这里返回 null 让上层给出「打开失败」提示，
 * 而不是让点击行为直接闪退。
 */
private fun fileViewUri(context: Context, file: File): Uri? {
    if (!file.exists()) {
        return null
    }
    return runCatching {
        FileProvider.getUriForFile(context, "${context.packageName}.fileprovider", file)
    }.getOrNull()
}

internal fun needsLegacyWritePermission(): Boolean {
    return Build.VERSION.SDK_INT <= Build.VERSION_CODES.P
}

internal fun persistUriPermission(
    context: Context,
    uri: Uri,
    read: Boolean,
    write: Boolean
) {
    val flags = (if (read) Intent.FLAG_GRANT_READ_URI_PERMISSION else 0) or
        (if (write) Intent.FLAG_GRANT_WRITE_URI_PERMISSION else 0)
    if (flags == 0) {
        return
    }
    try {
        context.contentResolver.takePersistableUriPermission(
            uri,
            flags
        )
    } catch (_: SecurityException) {
        // Photo Picker 之类只授予只读持久化权限：请求写权限会整体被拒，
        // 降级为仅读重试，至少保住读权限（否则一条授权都持久化不上）。
        val readFlag = flags and Intent.FLAG_GRANT_READ_URI_PERMISSION
        if (readFlag != 0) {
            try {
                context.contentResolver.takePersistableUriPermission(uri, readFlag)
            } catch (_: SecurityException) {
                // Some providers do not grant persistable permissions.
            } catch (_: IllegalArgumentException) {
                // URI does not support persistable permissions.
            }
        }
    } catch (_: IllegalArgumentException) {
        // URI does not support persistable permissions.
    }
}

internal fun isImageSourceDecodable(
    context: Context,
    rawSource: String
): Boolean {
    val normalizedSource = rawSource.trim()
    if (normalizedSource.isBlank()) {
        return false
    }

    return runCatching {
        val inputStream = when {
            normalizedSource.startsWith("/") -> File(normalizedSource).inputStream()
            else -> {
                val uri = Uri.parse(normalizedSource)
                when (uri.scheme?.lowercase()) {
                    null -> File(normalizedSource).inputStream()
                    "file" -> {
                        val path = uri.path ?: return false
                        File(path).inputStream()
                    }
                    else -> context.contentResolver.openInputStream(uri)
                }
            }
        } ?: return false

        inputStream.use { stream ->
            val boundsOptions = BitmapFactory.Options().apply {
                inJustDecodeBounds = true
            }
            BitmapFactory.decodeStream(stream, null, boundsOptions)
            boundsOptions.outWidth > 0 && boundsOptions.outHeight > 0
        }
    }.getOrDefault(false)
}
