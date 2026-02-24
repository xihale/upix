package com.crossscale.data.repository

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import com.crossscale.native.NCNNEngine
import java.io.File
import java.io.IOException
import kotlin.math.sqrt

data class NativeProcessResult(
    val outputFile: File,
    val inputWidth: Int,
    val inputHeight: Int,
    val outputWidth: Int,
    val outputHeight: Int
)

class NativeImageProcessingRepository(
    context: Context,
    private val logRepository: LogRepository
) : ImageProcessingStore {
    private val appContext = context.applicationContext

    override fun openSession(
        modelName: String,
        modelBasePath: String,
        preferGpu: Boolean
    ): ImageProcessingSession? {
        val requestedDeviceType = if (preferGpu) DEVICE_TYPE_GPU else DEVICE_TYPE_CPU
        val enginePtr = runCatching {
            NCNNEngine.nativeInit(requestedDeviceType)
        }.getOrElse { throwable ->
            logRepository.error(
                "NativeImageProcessingRepository",
                "Failed to initialize native engine: ${throwable.message}"
            )
            return null
        }

        if (enginePtr == 0L) {
            logRepository.error(
                "NativeImageProcessingRepository",
                "Failed to initialize native engine pointer (model=$modelName)"
            )
            return null
        }

        val loadSuccess = runCatching {
            NCNNEngine.nativeLoadModel(enginePtr, modelBasePath)
        }.getOrElse { throwable ->
            logRepository.error(
                "NativeImageProcessingRepository",
                "Failed to load model $modelName: ${throwable.message}"
            )
            false
        }

        if (!loadSuccess) {
            NCNNEngine.nativeRelease(enginePtr)
            logRepository.error(
                "NativeImageProcessingRepository",
                "Native model load failed for $modelName ($modelBasePath)"
            )
            return null
        }

        val scaleFactor = runCatching {
            NCNNEngine.nativeGetScaleFactor(enginePtr)
        }.getOrDefault(DEFAULT_SCALE_FACTOR).coerceAtLeast(1)

        logRepository.info(
            "NativeImageProcessingRepository",
            "Native session ready: model=$modelName, scale=$scaleFactor"
        )

        return NativeImageProcessingSession(
            context = appContext,
            logRepository = logRepository,
            enginePtr = enginePtr,
            scaleFactor = scaleFactor
        )
    }

    class NativeImageProcessingSession(
        private val context: Context,
        private val logRepository: LogRepository,
        private var enginePtr: Long,
        private val scaleFactor: Int
    ) : ImageProcessingSession {
        override fun process(
            input: ResolvedImageInput,
            outputFile: File
        ): Result<NativeProcessResult> {
            return runCatching {
                val activeEnginePtr = enginePtr
                require(activeEnginePtr != 0L) { "Native session already closed" }

                val decoded = decodeInput(input.uri)
                requireNotNull(decoded) {
                    "Failed to decode image input: ${input.displayName}"
                }

                val outputBytes = NCNNEngine.nativeProcessImage(
                    activeEnginePtr,
                    decoded.rgbBytes,
                    decoded.width,
                    decoded.height,
                    RGB_CHANNELS
                )
                requireNotNull(outputBytes) {
                    "Native engine returned empty result: ${input.displayName}"
                }

                val dimensions = resolveOutputDimensions(
                    inputWidth = decoded.width,
                    inputHeight = decoded.height,
                    outputByteCount = outputBytes.size
                )
                requireNotNull(dimensions) {
                    "Invalid native output size ${outputBytes.size} bytes for ${input.displayName}"
                }

                writeOutputPng(
                    file = outputFile,
                    rgbBytes = outputBytes,
                    width = dimensions.first,
                    height = dimensions.second
                )

                NativeProcessResult(
                    outputFile = outputFile,
                    inputWidth = decoded.width,
                    inputHeight = decoded.height,
                    outputWidth = dimensions.first,
                    outputHeight = dimensions.second
                )
            }
        }

        override fun close() {
            if (enginePtr == 0L) {
                return
            }
            runCatching {
                NCNNEngine.nativeRelease(enginePtr)
            }.onFailure { throwable ->
                logRepository.warning(
                    "NativeImageProcessingRepository",
                    "Failed to release native engine: ${throwable.message}"
                )
            }
            enginePtr = 0L
        }

        private fun decodeInput(uri: Uri): DecodedRgbImage? {
            val bitmap = runCatching {
                context.contentResolver.openInputStream(uri)?.use { inputStream ->
                    BitmapFactory.decodeStream(inputStream)
                }
            }.onFailure { throwable ->
                logRepository.error(
                    "NativeImageProcessingRepository",
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

            val pixels = IntArray(width * height)
            argbBitmap.getPixels(
                pixels,
                0,
                width,
                0,
                0,
                width,
                height
            )
            argbBitmap.recycle()

            val rgbBytes = ByteArray(width * height * RGB_CHANNELS)
            var writeIndex = 0
            pixels.forEach { pixel ->
                rgbBytes[writeIndex++] = ((pixel shr 16) and 0xFF).toByte()
                rgbBytes[writeIndex++] = ((pixel shr 8) and 0xFF).toByte()
                rgbBytes[writeIndex++] = (pixel and 0xFF).toByte()
            }

            return DecodedRgbImage(
                width = width,
                height = height,
                rgbBytes = rgbBytes
            )
        }

        private fun resolveOutputDimensions(
            inputWidth: Int,
            inputHeight: Int,
            outputByteCount: Int
        ): Pair<Int, Int>? {
            if (outputByteCount <= 0 || outputByteCount % RGB_CHANNELS != 0) {
                return null
            }

            val outputPixels = outputByteCount / RGB_CHANNELS
            val expectedWidth = inputWidth * scaleFactor
            val expectedHeight = inputHeight * scaleFactor
            if (expectedWidth > 0 && expectedHeight > 0 && expectedWidth * expectedHeight == outputPixels) {
                return expectedWidth to expectedHeight
            }

            if (expectedWidth > 0 && outputPixels % expectedWidth == 0) {
                val inferredHeight = outputPixels / expectedWidth
                if (inferredHeight > 0) {
                    return expectedWidth to inferredHeight
                }
            }

            val sideLength = sqrt(outputPixels.toDouble()).toInt()
            return if (sideLength * sideLength == outputPixels) {
                sideLength to sideLength
            } else {
                null
            }
        }

        private fun writeOutputPng(
            file: File,
            rgbBytes: ByteArray,
            width: Int,
            height: Int
        ) {
            val expectedSize = width * height * RGB_CHANNELS
            require(rgbBytes.size == expectedSize) {
                "Output pixel buffer size mismatch: expected=$expectedSize actual=${rgbBytes.size}"
            }

            val pixels = IntArray(width * height)
            var readIndex = 0
            for (pixelIndex in pixels.indices) {
                val r = rgbBytes[readIndex++].toInt() and 0xFF
                val g = rgbBytes[readIndex++].toInt() and 0xFF
                val b = rgbBytes[readIndex++].toInt() and 0xFF
                pixels[pixelIndex] = (0xFF shl 24) or (r shl 16) or (g shl 8) or b
            }

            val bitmap = Bitmap.createBitmap(pixels, width, height, Bitmap.Config.ARGB_8888)
            try {
                file.parentFile?.mkdirs()
                val writeSucceeded = file.outputStream().use { output ->
                    bitmap.compress(Bitmap.CompressFormat.PNG, PNG_COMPRESSION_QUALITY, output)
                }
                if (!writeSucceeded) {
                    throw IOException("Bitmap compress returned false")
                }
            } finally {
                bitmap.recycle()
            }
        }

        private data class DecodedRgbImage(
            val width: Int,
            val height: Int,
            val rgbBytes: ByteArray
        )
    }

    companion object {
        private const val DEVICE_TYPE_CPU = 0
        private const val DEVICE_TYPE_GPU = 1
        private const val RGB_CHANNELS = 3
        private const val DEFAULT_SCALE_FACTOR = 2
        private const val PNG_COMPRESSION_QUALITY = 100
    }
}
