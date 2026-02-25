package com.crossscale.network

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.Environment
import com.crossscale.data.repository.LogRepository
import com.crossscale.data.repository.NativeProcessResult
import com.crossscale.data.repository.ResolvedImageInput
import com.crossscale.native.NCNNEngine
import com.crossscale.native.NativeWorker
import java.io.File
import java.io.FileOutputStream

class CoordinatorClient(
    context: Context,
    private val logRepository: LogRepository
) {
    private val appContext = context.applicationContext
    private val modelsDirectory = appContext.getExternalFilesDir(Environment.DIRECTORY_DOCUMENTS)
        ?.let { directory -> File(directory, "models") }
        ?: File(appContext.filesDir, "models")

    private var worker: NativeWorker? = null
    private var connectedEndpoint: RemoteEndpoint? = null

    fun connect(host: String, port: Int, authKey: String): Boolean {
        val normalizedHost = host.trim()
        val normalizedAuthKey = authKey.trim()
        if (normalizedHost.isBlank() || port <= 0 || normalizedAuthKey.isBlank()) {
            logRepository.error("CoordinatorClient", "Invalid connection params")
            return false
        }

        val previousEndpoint = connectedEndpoint
        if (
            previousEndpoint != null &&
            (previousEndpoint.host != normalizedHost || previousEndpoint.port != port)
        ) {
            disconnect()
        }

        if (!modelsDirectory.exists() && !modelsDirectory.mkdirs()) {
            logRepository.error("CoordinatorClient", "Failed to create models directory: ${modelsDirectory.absolutePath}")
            return false
        }

        val activeWorker = worker ?: NativeWorker().also { worker = it }
        val connected = try {
            activeWorker.connect(
                host = normalizedHost,
                port = port,
                authKey = normalizedAuthKey,
                modelsDirectory = modelsDirectory.absolutePath
            )
        } catch (e: UnsatisfiedLinkError) {
            logRepository.error("CoordinatorClient", "Native bridge unavailable: ${e.message}")
            false
        } catch (e: Exception) {
            logRepository.error("CoordinatorClient", "Worker connect failed: ${e.message}")
            false
        }

        if (connected) {
            connectedEndpoint = RemoteEndpoint(
                host = normalizedHost,
                port = port,
                authKey = normalizedAuthKey
            )
            logRepository.info("CoordinatorClient", "Connected to $normalizedHost:$port")
        } else {
            connectedEndpoint = null
            val reason = runCatching { activeWorker.lastError() }
                .getOrNull()
                .orEmpty()
                .trim()
            if (reason.isNotBlank()) {
                logRepository.warning("CoordinatorClient", "Connect rejected by native worker: $reason")
            } else {
                logRepository.warning("CoordinatorClient", "Connect rejected by native worker")
            }
            runCatching {
                activeWorker.disconnect()
                activeWorker.close()
            }
            worker = null
        }
        return connected
    }

    fun isConnected(): Boolean {
        val activeWorker = worker ?: return false
        val linked = runCatching { activeWorker.isConnected() }.getOrElse { false }
        if (!linked && connectedEndpoint != null) {
            logRepository.warning("CoordinatorClient", "Coordinator link dropped, releasing worker resources")
            disconnect()
        }
        return linked
    }

    fun isConnectedTo(host: String, port: Int): Boolean {
        val endpoint = connectedEndpoint ?: return false
        if (!isConnected()) {
            return false
        }
        return endpoint.host == host.trim() && endpoint.port == port
    }

    fun activeDeviceId(): String? {
        val endpoint = connectedEndpoint ?: return null
        return "${endpoint.host}:${endpoint.port}"
    }

    fun processInputRemotely(
        input: ResolvedImageInput,
        modelName: String,
        tileSize: Int,
        tileOverlap: Int,
        outputFile: File
    ): Result<NativeProcessResult> {
        val endpoint = connectedEndpoint
            ?: return Result.failure(IllegalStateException("No connected coordinator endpoint"))
        if (!isConnected()) {
            return Result.failure(IllegalStateException("Coordinator link is not active"))
        }

        val decodedInput = decodeInput(input.uri)
            ?: return Result.failure(IllegalStateException("Failed to decode image input: ${input.displayName}"))

        val outputFormat = outputFormatFor(outputFile)
        val startedAt = System.currentTimeMillis()
        val encodedOutput = NCNNEngine.nativeProcessImageDistributed(
            host = endpoint.host,
            port = endpoint.port,
            authKey = endpoint.authKey,
            modelName = modelName,
            tileSize = tileSize.coerceAtLeast(MIN_TILE_SIZE),
            tileOverlap = tileOverlap.coerceAtLeast(0),
            inputImage = decodedInput.rgbBytes,
            width = decodedInput.width,
            height = decodedInput.height,
            channels = RGB_CHANNELS,
            outputFormat = outputFormat,
            outputQuality = DEFAULT_OUTPUT_QUALITY
        )

        if (encodedOutput == null || encodedOutput.isEmpty()) {
            val reason = NCNNEngine.nativeGetLastError()
                ?.trim()
                .orEmpty()
            val message = if (reason.isNotBlank()) {
                "Distributed processing failed: $reason"
            } else {
                "Distributed processing failed without native details"
            }
            logRepository.error("CoordinatorClient", message)
            return Result.failure(IllegalStateException(message))
        }

        val outputBounds = decodeOutputBounds(encodedOutput)
            ?: return Result.failure(IllegalStateException("Distributed output is not decodable"))

        val writeResult = writeOutputBytes(outputFile, encodedOutput)
        if (writeResult.isFailure) {
            return Result.failure(
                writeResult.exceptionOrNull()
                    ?: IllegalStateException("Failed to persist distributed output")
            )
        }

        val elapsedMs = System.currentTimeMillis() - startedAt
        logRepository.info(
            "CoordinatorClient",
            "Remote processed ${input.displayName} in ${elapsedMs}ms via ${endpoint.host}:${endpoint.port}"
        )

        return Result.success(
            NativeProcessResult(
                outputFile = outputFile,
                inputWidth = decodedInput.width,
                inputHeight = decodedInput.height,
                outputWidth = outputBounds.first,
                outputHeight = outputBounds.second,
                processorLabel = "${endpoint.host}:${endpoint.port}"
            )
        )
    }

    fun disconnectIfMatches(host: String, port: Int): Boolean {
        val endpoint = connectedEndpoint ?: return false
        if (endpoint.host != host.trim() || endpoint.port != port) {
            return false
        }
        disconnect()
        return true
    }

    fun disconnect() {
        val endpoint = connectedEndpoint
        worker?.disconnect()
        worker?.close()
        worker = null
        connectedEndpoint = null
        if (endpoint != null) {
            logRepository.info("CoordinatorClient", "Disconnected from ${endpoint.host}:${endpoint.port}")
        } else {
            logRepository.info("CoordinatorClient", "Disconnected")
        }
    }

    fun release() {
        disconnect()
    }

    private fun decodeInput(uri: Uri): DecodedRgbImage? {
        val bitmap = runCatching {
            appContext.contentResolver.openInputStream(uri)?.use { inputStream ->
                BitmapFactory.decodeStream(inputStream)
            }
        }.onFailure { throwable ->
            logRepository.error(
                "CoordinatorClient",
                "Unable to read input URI $uri: ${throwable.message}"
            )
        }.getOrNull() ?: return null

        val argbBitmap = if (bitmap.config == Bitmap.Config.ARGB_8888) {
            bitmap
        } else {
            val converted = bitmap.copy(Bitmap.Config.ARGB_8888, false)
            bitmap.recycle()
            converted ?: return null
        }

        val width = argbBitmap.width
        val height = argbBitmap.height
        if (width <= 0 || height <= 0) {
            argbBitmap.recycle()
            return null
        }

        val rgbByteCount = width.toLong() * height.toLong() * RGB_CHANNELS
        if (rgbByteCount <= 0L || rgbByteCount > Int.MAX_VALUE.toLong()) {
            argbBitmap.recycle()
            return null
        }

        val rgbBytes = ByteArray(rgbByteCount.toInt())
        val rowPixels = IntArray(width)
        var writeIndex = 0
        for (row in 0 until height) {
            argbBitmap.getPixels(
                rowPixels,
                0,
                width,
                0,
                row,
                width,
                1
            )
            for (column in 0 until width) {
                val pixel = rowPixels[column]
                rgbBytes[writeIndex++] = ((pixel shr 16) and 0xFF).toByte()
                rgbBytes[writeIndex++] = ((pixel shr 8) and 0xFF).toByte()
                rgbBytes[writeIndex++] = (pixel and 0xFF).toByte()
            }
        }
        argbBitmap.recycle()

        return DecodedRgbImage(
            width = width,
            height = height,
            rgbBytes = rgbBytes
        )
    }

    private fun decodeOutputBounds(encodedBytes: ByteArray): Pair<Int, Int>? {
        val options = BitmapFactory.Options().apply {
            inJustDecodeBounds = true
        }
        BitmapFactory.decodeByteArray(encodedBytes, 0, encodedBytes.size, options)
        if (options.outWidth <= 0 || options.outHeight <= 0) {
            return null
        }
        return options.outWidth to options.outHeight
    }

    private fun writeOutputBytes(file: File, encodedBytes: ByteArray): Result<Unit> {
        return runCatching {
            val parent = file.parentFile
            if (parent != null && !parent.exists() && !parent.mkdirs()) {
                throw IllegalStateException("Failed to create output directory: ${parent.absolutePath}")
            }
            FileOutputStream(file, false).use { outputStream ->
                outputStream.write(encodedBytes)
                outputStream.flush()
            }
        }
    }

    private fun outputFormatFor(outputFile: File): String {
        return when (outputFile.extension.lowercase()) {
            "png" -> "png"
            "webp" -> "webp"
            else -> "jpg"
        }
    }

    private data class DecodedRgbImage(
        val width: Int,
        val height: Int,
        val rgbBytes: ByteArray
    )

    private data class RemoteEndpoint(
        val host: String,
        val port: Int,
        val authKey: String
    )

    private companion object {
        private const val RGB_CHANNELS = 3
        private const val DEFAULT_OUTPUT_QUALITY = 95
        private const val MIN_TILE_SIZE = 16
    }
}
