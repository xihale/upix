package com.crossscale.data.repository

internal const val DEFAULT_OUTPUT_FORMAT = "png"
internal const val AVIF_OUTPUT_FORMAT = "avif"

/** Native encode format requested from a file extension. */
internal fun outputFormatForExtension(extension: String): String {
    val normalized = extension.trim().lowercase().trimStart('.')
    return when (normalized) {
        "png" -> "png"
        "jpg" -> "jpg"
        "jpeg" -> "jpeg"
        "webp" -> "webp"
        "avif" -> AVIF_OUTPUT_FORMAT
        else -> DEFAULT_OUTPUT_FORMAT
    }
}

internal fun normalizedOutputExtension(displayName: String): String {
    return when (displayName.substringAfterLast('.', "").lowercase()) {
        "jpg" -> "jpg"
        "jpeg" -> "jpeg"
        "webp" -> "webp"
        "png" -> "png"
        "avif" -> "avif"
        else -> DEFAULT_OUTPUT_FORMAT
    }
}

/**
 * Output file extension for one item.
 *
 * Local device cannot encode AVIF, so FOLLOW + `.avif` input falls back to PNG
 * unless [writeAvif] (remote PC). Overwrite keeps the original extension so
 * the encode path can fail loudly instead of writing PNG bytes into a `.avif`.
 */
internal fun resolveOutputExtension(
    displayName: String,
    requested: OutputImageFormat,
    overwriteOriginal: Boolean,
    writeAvif: Boolean
): String {
    if (overwriteOriginal) {
        return normalizedOutputExtension(displayName)
    }
    val extension = requested.forcedExtension ?: normalizedOutputExtension(displayName)
    if (extension == AVIF_OUTPUT_FORMAT && !writeAvif && requested != OutputImageFormat.AVIF) {
        return DEFAULT_OUTPUT_FORMAT
    }
    return extension
}
