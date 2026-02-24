package com.crossscale.data.repository

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import com.crossscale.native.NCNNEngine
import java.io.File

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
        var selectedHandle = createEngineHandle(
            modelName = modelName,
            modelBasePath = modelBasePath,
            deviceType = requestedDeviceType
        ) ?: return null

        if (preferGpu) {
            val validationResult = validateGpuSession(
                gpuHandle = selectedHandle
            )
            if (!validationResult.isStable) {
                logRepository.error(
                    "NativeImageProcessingRepository",
                    "GPU output validation failed (${validationResult.reason}), aborting GPU session without CPU fallback"
                )
                releaseEngine(
                    enginePtr = selectedHandle.enginePtr,
                    errorPrefix = "Failed to release unstable GPU engine"
                )
                return null
            } else {
                logRepository.info(
                    "NativeImageProcessingRepository",
                    "GPU output validation passed"
                )
            }
        }

        logRepository.info(
            "NativeImageProcessingRepository",
            "Native session ready: model=$modelName, scale=${selectedHandle.scaleFactor}, " +
                "mode=${deviceLabel(selectedHandle.deviceType)}"
        )

        return NativeImageProcessingSession(
            context = appContext,
            logRepository = logRepository,
            enginePtr = selectedHandle.enginePtr,
            scaleFactor = selectedHandle.scaleFactor,
            activeDeviceType = selectedHandle.deviceType
        )
    }

    private data class NativeEngineHandle(
        val enginePtr: Long,
        val scaleFactor: Int,
        val deviceType: Int
    )

    private data class GpuValidationResult(
        val isStable: Boolean,
        val reason: String
    )

    private fun createEngineHandle(
        modelName: String,
        modelBasePath: String,
        deviceType: Int
    ): NativeEngineHandle? {
        val enginePtr = runCatching {
            NCNNEngine.nativeInit(deviceType)
        }.getOrElse { throwable ->
            logRepository.error(
                "NativeImageProcessingRepository",
                "Failed to initialize native engine (${deviceLabel(deviceType)}): ${throwable.message}"
            )
            return null
        }

        if (enginePtr == 0L) {
            logRepository.error(
                "NativeImageProcessingRepository",
                "Failed to initialize native engine pointer (model=$modelName, mode=${deviceLabel(deviceType)})"
            )
            return null
        }

        val loadSuccess = runCatching {
            NCNNEngine.nativeLoadModel(enginePtr, modelBasePath)
        }.getOrElse { throwable ->
            logRepository.error(
                "NativeImageProcessingRepository",
                "Failed to load model $modelName (${deviceLabel(deviceType)}): ${throwable.message}"
            )
            false
        }
        if (!loadSuccess) {
            releaseEngine(
                enginePtr = enginePtr,
                errorPrefix = "Failed to release engine after load failure"
            )
            logRepository.error(
                "NativeImageProcessingRepository",
                "Native model load failed for $modelName ($modelBasePath, mode=${deviceLabel(deviceType)})"
            )
            return null
        }

        val scaleFactor = runCatching {
            NCNNEngine.nativeGetScaleFactor(enginePtr)
        }.getOrDefault(DEFAULT_SCALE_FACTOR).coerceAtLeast(1)

        return NativeEngineHandle(
            enginePtr = enginePtr,
            scaleFactor = scaleFactor,
            deviceType = deviceType
        )
    }

    private fun releaseEngine(
        enginePtr: Long,
        errorPrefix: String
    ) {
        runCatching {
            NCNNEngine.nativeRelease(enginePtr)
        }.onFailure { throwable ->
            logRepository.warning(
                "NativeImageProcessingRepository",
                "$errorPrefix: ${throwable.message}"
            )
        }
    }

    private fun validateGpuSession(
        gpuHandle: NativeEngineHandle
    ): GpuValidationResult {
        val probeInput = buildValidationInput()
        val probeOutputGpu = processValidationInput(
            enginePtr = gpuHandle.enginePtr,
            inputBytes = probeInput,
            width = GPU_VALIDATION_INPUT_SIZE,
            height = GPU_VALIDATION_INPUT_SIZE
        ) ?: return GpuValidationResult(
            isStable = false,
            reason = "GPU probe returned empty output"
        )

        val gpuBitmap = decodeValidationBitmap(probeOutputGpu)
            ?: return GpuValidationResult(
                isStable = false,
                reason = "GPU probe decode failed"
            )

        return try {
            val expectedWidth = GPU_VALIDATION_INPUT_SIZE * gpuHandle.scaleFactor
            val expectedHeight = GPU_VALIDATION_INPUT_SIZE * gpuHandle.scaleFactor
            if (gpuBitmap.width != expectedWidth || gpuBitmap.height != expectedHeight) {
                GpuValidationResult(
                    isStable = false,
                    reason = "Unexpected GPU output size ${gpuBitmap.width}x${gpuBitmap.height} (expected ${expectedWidth}x${expectedHeight})"
                )
            } else {
                GpuValidationResult(
                    isStable = true,
                    reason = "ok"
                )
            }
        } finally {
            gpuBitmap.recycle()
        }
    }

    private fun processValidationInput(
        enginePtr: Long,
        inputBytes: ByteArray,
        width: Int,
        height: Int
    ): ByteArray? {
        return runCatching {
            NCNNEngine.nativeProcessImage(
                enginePtr,
                inputBytes,
                width,
                height,
                RGB_CHANNELS,
                VALIDATION_OUTPUT_FORMAT,
                DEFAULT_OUTPUT_QUALITY
            )
        }.onFailure { throwable ->
            logRepository.warning(
                "NativeImageProcessingRepository",
                "Validation probe failed: ${throwable.message}"
            )
        }.getOrNull()
    }

    private fun decodeValidationBitmap(encodedBytes: ByteArray): Bitmap? {
        return runCatching {
            BitmapFactory.decodeByteArray(encodedBytes, 0, encodedBytes.size)
        }.getOrNull()
    }

    private fun buildValidationInput(): ByteArray {
        val width = GPU_VALIDATION_INPUT_SIZE
        val height = GPU_VALIDATION_INPUT_SIZE
        val data = ByteArray(width * height * RGB_CHANNELS)
        var index = 0
        for (y in 0 until height) {
            for (x in 0 until width) {
                val r = (x * 255) / (width - 1)
                val g = (y * 255) / (height - 1)
                val checker = if (((x / 8) + (y / 8)) % 2 == 0) 60 else 0
                val b = (((255 - r) + checker) / 2).coerceIn(0, 255)
                data[index++] = r.toByte()
                data[index++] = g.toByte()
                data[index++] = b.toByte()
            }
        }
        return data
    }

    private fun deviceLabel(deviceType: Int): String {
        return if (deviceType == DEVICE_TYPE_GPU) "GPU" else "CPU"
    }

    private class NativeImageProcessingSession(
        private val context: Context,
        private val logRepository: LogRepository,
        private var enginePtr: Long,
        private var scaleFactor: Int,
        private var activeDeviceType: Int
    ) : ImageProcessingSession {

        override fun process(
            input: ResolvedImageInput,
            outputFile: File
        ): Result<NativeProcessResult> {
            return processInternal(
                input = input,
                outputFile = outputFile
            )
        }

        private fun processInternal(
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

                val expectedRgbBytes = decoded.width * decoded.height * RGB_CHANNELS
                require(decoded.rgbBytes.size == expectedRgbBytes) {
                    "Decoded buffer size mismatch for ${input.displayName}: " +
                        "actual=${decoded.rgbBytes.size}, expected=$expectedRgbBytes"
                }
                val outputFormat = outputFormatFor(outputFile)
                logRepository.info(
                    "NativeImageProcessingRepository",
                    "Start native processing: input=${input.displayName}, uri=${input.uri}, " +
                        "size=${decoded.width}x${decoded.height}, rgbBytes=${decoded.rgbBytes.size}, " +
                        "output=${outputFile.absolutePath}, format=$outputFormat"
                )

                val outputBytes = NCNNEngine.nativeProcessImage(
                    activeEnginePtr,
                    decoded.rgbBytes,
                    decoded.width,
                    decoded.height,
                    RGB_CHANNELS,
                    outputFormat,
                    DEFAULT_OUTPUT_QUALITY
                )
                requireNotNull(outputBytes) {
                    "Native engine returned empty result: ${input.displayName}, " +
                        "size=${decoded.width}x${decoded.height}, format=$outputFormat"
                }

                logRepository.info(
                    "NativeImageProcessingRepository",
                    "Native processing completed: input=${input.displayName}, outputBytes=${outputBytes.size}"
                )

                val outputWidth = decoded.width * scaleFactor
                val outputHeight = decoded.height * scaleFactor
                validateEncodedOutput(
                    encodedBytes = outputBytes,
                    expectedWidth = outputWidth,
                    expectedHeight = outputHeight,
                    displayName = input.displayName
                )

                writeOutputBytes(
                    file = outputFile,
                    encodedBytes = outputBytes
                )

                NativeProcessResult(
                    outputFile = outputFile,
                    inputWidth = decoded.width,
                    inputHeight = decoded.height,
                    outputWidth = outputWidth,
                    outputHeight = outputHeight
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

            val pixelCount = width.toLong() * height.toLong()
            val rgbByteCount = pixelCount * RGB_CHANNELS
            if (rgbByteCount <= 0L || rgbByteCount > Int.MAX_VALUE.toLong()) {
                logRepository.error(
                    "NativeImageProcessingRepository",
                    "Input image is too large to allocate RGB buffer: $width x $height"
                )
                argbBitmap.recycle()
                return null
            }

            if (!hasSufficientHeapForRgbBuffer(width = width, rgbByteCount = rgbByteCount)) {
                val runtime = Runtime.getRuntime()
                val used = runtime.totalMemory() - runtime.freeMemory()
                val available = (runtime.maxMemory() - used).coerceAtLeast(0L)
                logRepository.error(
                    "NativeImageProcessingRepository",
                    "Insufficient heap before RGB conversion: $width x $height, " +
                        "required=${rgbByteCount}B, available=${available}B"
                )
                argbBitmap.recycle()
                return null
            }

            val rgbBytes = runCatching {
                ByteArray(rgbByteCount.toInt())
            }.onFailure { throwable ->
                logRepository.error(
                    "NativeImageProcessingRepository",
                    "Failed to allocate RGB buffer for $uri (${width}x${height}): ${throwable.message}"
                )
            }.getOrNull() ?: run {
                argbBitmap.recycle()
                return null
            }

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

        private fun hasSufficientHeapForRgbBuffer(
            width: Int,
            rgbByteCount: Long
        ): Boolean {
            val runtime = Runtime.getRuntime()
            val used = runtime.totalMemory() - runtime.freeMemory()
            val available = (runtime.maxMemory() - used).coerceAtLeast(0L)
            val estimatedAdditionalBytes = rgbByteCount + (width.toLong() * Int.SIZE_BYTES)
            val safeBudget = (available * MAX_RGB_BUFFER_HEAP_RATIO).toLong()
            return estimatedAdditionalBytes <= safeBudget
        }

        private fun validateEncodedOutput(
            encodedBytes: ByteArray,
            expectedWidth: Int,
            expectedHeight: Int,
            displayName: String
        ) {
            require(encodedBytes.isNotEmpty()) { "Output image buffer is empty: $displayName" }
            val options = BitmapFactory.Options().apply {
                inJustDecodeBounds = true
            }
            BitmapFactory.decodeByteArray(encodedBytes, 0, encodedBytes.size, options)
            val decodedWidth = options.outWidth
            val decodedHeight = options.outHeight
            require(decodedWidth > 0 && decodedHeight > 0) {
                "Encoded output is not decodable: $displayName"
            }
            require(decodedWidth == expectedWidth && decodedHeight == expectedHeight) {
                "Output dimensions mismatch for $displayName: actual=${decodedWidth}x${decodedHeight}, " +
                    "expected=${expectedWidth}x${expectedHeight}"
            }
        }

        private fun writeOutputBytes(
            file: File,
            encodedBytes: ByteArray
        ) {
            require(encodedBytes.isNotEmpty()) { "Output image buffer is empty" }
            file.parentFile?.mkdirs()
            file.outputStream().use { output ->
                output.write(encodedBytes)
            }
        }

        private fun outputFormatFor(file: File): String {
            val extension = file.extension.trim().lowercase()
            return when (extension) {
                "png" -> "png"
                "jpg" -> "jpg"
                "jpeg" -> "jpeg"
                "webp" -> "webp"
                else -> DEFAULT_OUTPUT_FORMAT
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
        private const val DEFAULT_OUTPUT_FORMAT = "png"
        private const val VALIDATION_OUTPUT_FORMAT = "png"
        private const val DEFAULT_OUTPUT_QUALITY = 90
        private const val MAX_RGB_BUFFER_HEAP_RATIO = 0.85
        private const val GPU_VALIDATION_INPUT_SIZE = 96
    }
}
