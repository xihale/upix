package com.upix.data.repository

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import com.upix.R
import com.upix.nativelib.NCNNEngine
import com.upix.nativelib.NativeCapabilities
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import kotlin.math.sqrt

data class NativeProcessResult(
    val outputFile: File,
    val inputWidth: Int,
    val inputHeight: Int,
    val outputWidth: Int,
    val outputHeight: Int,
)

class NativeImageProcessingRepository(
    context: Context,
    private val logRepository: LogRepository
) : ImageProcessingStore {
    private val appContext = context.applicationContext

    // 高级编码参数：MainViewModel 在初始化/设置变更时推送；快照语义，
    // session.process() 每张图读取一次（run 中途改设置不影响进行中的批次）。
    @Volatile
    private var encoderSettings = EncoderSettings()

    override fun updateEncoderSettings(settings: EncoderSettings) {
        encoderSettings = settings
    }

    @Synchronized
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
                logRepository.warning(
                    "NativeImageProcessingRepository",
                    "GPU output validation failed (${validationResult.reason}), falling back to CPU session"
                )
                releaseEngine(
                    enginePtr = selectedHandle.enginePtr,
                    errorPrefix = "Failed to release unstable GPU engine"
                )
                // Fall back to CPU instead of aborting the whole session.
                // The GPU may still be usable for larger real-world images, but the
                // synthetic probe is conservative — never block the user over a probe.
                selectedHandle = createEngineHandle(
                    modelName = modelName,
                    modelBasePath = modelBasePath,
                    deviceType = DEVICE_TYPE_CPU
                ) ?: return null
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
            // 编码参数在 session 建立时快照：run 中途改设置不影响进行中的批次。
            encoderSettings = encoderSettings
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
            val nativeError = runCatching {
                NCNNEngine.nativeGetLastError().orEmpty()
            }.getOrDefault("")
            releaseEngine(
                enginePtr = enginePtr,
                errorPrefix = "Failed to release engine after load failure"
            )
            logRepository.error(
                "NativeImageProcessingRepository",
                if (nativeError.isBlank()) {
                    "Native model load failed for $modelName ($modelBasePath, mode=${deviceLabel(deviceType)})"
                } else {
                    "Native model load failed for $modelName ($modelBasePath, mode=${deviceLabel(deviceType)}): $nativeError"
                }
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
        val firstProbeOutput = processValidationInput(
            enginePtr = gpuHandle.enginePtr,
            inputBytes = probeInput,
            width = GPU_VALIDATION_INPUT_SIZE,
            height = GPU_VALIDATION_INPUT_SIZE
        ) ?: return GpuValidationResult(
            isStable = false,
            reason = "GPU probe returned empty output"
        )

        val secondProbeOutput = processValidationInput(
            enginePtr = gpuHandle.enginePtr,
            inputBytes = probeInput,
            width = GPU_VALIDATION_INPUT_SIZE,
            height = GPU_VALIDATION_INPUT_SIZE
        ) ?: return GpuValidationResult(
            isStable = false,
            reason = "GPU probe second pass returned empty output"
        )

        val firstProbeBitmap = decodeValidationBitmap(firstProbeOutput)
            ?: return GpuValidationResult(
                isStable = false,
                reason = "GPU probe first decode failed"
            )
        val secondProbeBitmap = decodeValidationBitmap(secondProbeOutput)
            ?: return GpuValidationResult(
                isStable = false,
                reason = "GPU probe second decode failed"
            )

        return try {
            val expectedWidth = GPU_VALIDATION_INPUT_SIZE * gpuHandle.scaleFactor
            val expectedHeight = GPU_VALIDATION_INPUT_SIZE * gpuHandle.scaleFactor
            if (
                firstProbeBitmap.width != expectedWidth ||
                firstProbeBitmap.height != expectedHeight ||
                secondProbeBitmap.width != expectedWidth ||
                secondProbeBitmap.height != expectedHeight
            ) {
                GpuValidationResult(
                    isStable = false,
                    reason = "Unexpected GPU output size first=${firstProbeBitmap.width}x${firstProbeBitmap.height}, second=${secondProbeBitmap.width}x${secondProbeBitmap.height}, expected=${expectedWidth}x${expectedHeight}"
                )
            } else {
                val firstMetrics = computeVisualSignal(firstProbeBitmap)
                val secondMetrics = computeVisualSignal(secondProbeBitmap)
                logRepository.debug(
                    "NativeImageProcessingRepository",
                    "GPU probe metrics: first(range=${firstMetrics.dynamicRange}, std=${"%.2f".format(firstMetrics.stdDev)}, mean=${"%.1f".format(firstMetrics.mean)}), " +
                        "second(range=${secondMetrics.dynamicRange}, std=${"%.2f".format(secondMetrics.stdDev)}, mean=${"%.1f".format(secondMetrics.mean)}), " +
                        "thresholds(range>=$GPU_VALIDATION_MIN_DYNAMIC_RANGE, std>=$GPU_VALIDATION_MIN_STD_DEV)"
                )
                // Save probe outputs for post-mortem analysis when they fail validation.
                if (!firstMetrics.hasSignal || !secondMetrics.hasSignal) {
                    saveProbeForDiagnosis(firstProbeOutput, secondProbeOutput)
                }
                when {
                    !firstMetrics.hasSignal -> GpuValidationResult(
                        isStable = false,
                        reason = "GPU probe output insufficient dynamic range " +
                            "(range=${firstMetrics.dynamicRange}, std=${"%.2f".format(firstMetrics.stdDev)}, mean=${"%.1f".format(firstMetrics.mean)})"
                    )
                    !secondMetrics.hasSignal -> GpuValidationResult(
                        isStable = false,
                        reason = "GPU probe second output insufficient dynamic range " +
                            "(range=${secondMetrics.dynamicRange}, std=${"%.2f".format(secondMetrics.stdDev)}, mean=${"%.1f".format(secondMetrics.mean)})"
                    )
                    else -> GpuValidationResult(
                        isStable = true,
                        reason = "ok"
                    )
                }
            }
        } finally {
            firstProbeBitmap.recycle()
            secondProbeBitmap.recycle()
        }
    }

    private data class VisualSignalMetrics(
        val dynamicRange: Int,
        val stdDev: Double,
        val mean: Double,
        val hasSignal: Boolean
    )

    private fun computeVisualSignal(bitmap: Bitmap): VisualSignalMetrics {
        val width = bitmap.width
        val height = bitmap.height
        if (width <= 0 || height <= 0) {
            return VisualSignalMetrics(0, 0.0, 0.0, false)
        }

        val pixelCount = width * height
        val pixels = IntArray(pixelCount)
        bitmap.getPixels(pixels, 0, width, 0, 0, width, height)

        // Single pass: accumulate sum and sum-of-squares, then derive
        // mean/variance (E[x^2] - E[x]^2) from the two accumulators.
        var minLuma = 255
        var maxLuma = 0
        var sum = 0.0
        var sumOfSquares = 0.0
        for (pixel in pixels) {
            val r = (pixel shr 16) and 0xFF
            val g = (pixel shr 8) and 0xFF
            val b = pixel and 0xFF
            val luma = (r * 299 + g * 587 + b * 114) / 1000
            minLuma = minOf(minLuma, luma)
            maxLuma = maxOf(maxLuma, luma)
            sum += luma.toDouble()
            sumOfSquares += luma.toDouble() * luma
        }

        val mean = sum / pixelCount
        val meanOfSquares = sumOfSquares / pixelCount
        val stdDev = sqrt((meanOfSquares - mean * mean).coerceAtLeast(0.0))
        val dynamicRange = maxLuma - minLuma
        val hasSignal = dynamicRange >= GPU_VALIDATION_MIN_DYNAMIC_RANGE &&
            stdDev >= GPU_VALIDATION_MIN_STD_DEV
        return VisualSignalMetrics(dynamicRange, stdDev, mean, hasSignal)
    }

    private fun processValidationInput(
        enginePtr: Long,
        inputBytes: ByteArray,
        width: Int,
        height: Int
    ): ByteArray? {
        val result = runCatching {
            NCNNEngine.nativeProcessImage(
                enginePtr,
                inputBytes,
                width,
                height,
                RGB_CHANNELS,
                VALIDATION_OUTPUT_FORMAT,
                DEFAULT_OUTPUT_QUALITY,
                ""
            )
        }
        result.onFailure { throwable ->
            logRepository.warning(
                "NativeImageProcessingRepository",
                "Validation probe failed: ${throwable.message}"
            )
        }
        // nativeProcessImage signals engine failures by returning null (not by
        // throwing); the actual reason is only in nativeGetLastError(). Without
        // this, the "GPU probe returned empty output" fallback warning carries
        // no root cause and the failure is only visible in logcat.
        val output = result.getOrNull()
        if (output == null) {
            val nativeError = runCatching {
                NCNNEngine.nativeGetLastError().orEmpty()
            }.getOrDefault("")
            logRepository.warning(
                "NativeImageProcessingRepository",
                if (nativeError.isBlank()) {
                    "Validation probe returned null (no native error recorded)"
                } else {
                    "Validation probe returned null: $nativeError"
                }
            )
        }
        return output
    }

    private fun decodeValidationBitmap(encodedBytes: ByteArray): Bitmap? {
        return runCatching {
            val decoded = BitmapFactory.decodeByteArray(encodedBytes, 0, encodedBytes.size)
                ?: return@runCatching null
            if (decoded.config == Bitmap.Config.ARGB_8888) {
                decoded
            } else {
                val converted = decoded.copy(Bitmap.Config.ARGB_8888, false)
                decoded.recycle()
                converted
            }
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

    /**
     * Dumps the failed probe PNGs to app cache so we can pull and inspect them.
     * Helps diagnose whether the GPU produced garbage, a constant image, or
     * just slightly-clipped output.
     */
    private fun saveProbeForDiagnosis(firstOutput: ByteArray, secondOutput: ByteArray) {
        runCatching {
            val dir = File(appContext.cacheDir, "gpu_probe").apply { mkdirs() }
            File(dir, "probe_first.png").writeBytes(firstOutput)
            File(dir, "probe_second.png").writeBytes(secondOutput)
            logRepository.info(
                "NativeImageProcessingRepository",
                "Saved failed GPU probe outputs to ${dir.absolutePath}"
            )
        }.onFailure { throwable ->
            logRepository.warning(
                "NativeImageProcessingRepository",
                "Failed to save probe outputs: ${throwable.message}"
            )
        }
    }

    private fun deviceLabel(deviceType: Int): String {
        return if (deviceType == DEVICE_TYPE_GPU) "GPU" else "CPU"
    }

    private class NativeImageProcessingSession(
        private val context: Context,
        private val logRepository: LogRepository,
        private var enginePtr: Long,
        private var scaleFactor: Int,
        private val encoderSettings: EncoderSettings
    ) : ImageProcessingSession {
        override fun process(
            input: ResolvedImageInput,
            outputFile: File
        ): Result<NativeProcessResult> = runCatching {
            val activeEnginePtr = enginePtr
            require(activeEnginePtr != 0L) { "Native session already closed" }

            val decoded = decodeInput(input)
            requireNotNull(decoded) {
                "Failed to decode image input: ${input.displayName}"
            }

            val expectedRgbBytes = decoded.width * decoded.height * RGB_CHANNELS
            require(decoded.rgbBytes.size == expectedRgbBytes) {
                "Decoded buffer size mismatch for ${input.displayName}: " +
                    "actual=${decoded.rgbBytes.size}, expected=$expectedRgbBytes"
            }
            val outputFormat = outputFormatFor(outputFile)
            // AVIF 本地编码仅在自带编码器的构建可用（-Pupix.avifEncoder=true）；
            // 精简构建保持旧行为——快速失败，引导改格式或连 PC。
            if (outputFormat == AVIF_OUTPUT_FORMAT && !NativeCapabilities.avifEncode) {
                require(false) {
                    "本地无法编码 AVIF（此构建未内置 AVIF 编码器）。" +
                        "请连接 PC 远程处理，或改用 PNG/JPEG/WebP"
                }
            }
            val settings = encoderSettings
            logRepository.info(
                "NativeImageProcessingRepository",
                "Start native processing: input=${input.displayName}, uri=${input.uri}, " +
                    "size=${decoded.width}x${decoded.height}, rgbBytes=${decoded.rgbBytes.size}, " +
                    "output=${outputFile.absolutePath}, format=$outputFormat, " +
                    "quality=${settings.normalizedQuality}"
            )

            val outputBytes = NCNNEngine.nativeProcessImage(
                activeEnginePtr,
                decoded.rgbBytes,
                decoded.width,
                decoded.height,
                RGB_CHANNELS,
                outputFormat,
                settings.normalizedQuality,
                settings.toNativeJson()
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
                outputHeight = outputHeight,
            )
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

        private fun decodeInput(input: ResolvedImageInput): DecodedRgbImage? {
            return decodeRgb(
                resolver = context.contentResolver,
                uri = input.uri,
                displayName = input.displayName
            ) { message ->
                logRepository.error("NativeImageProcessingRepository", message)
            }
        }

        private fun validateEncodedOutput(
            encodedBytes: ByteArray,
            expectedWidth: Int,
            expectedHeight: Int,
            displayName: String
        ) {
            require(encodedBytes.isNotEmpty()) { "Output image buffer is empty: $displayName" }
            val bounds = decodeEncodedImageBounds(encodedBytes)
            require(bounds != null) {
                "Encoded output is not decodable: $displayName"
            }
            val (decodedWidth, decodedHeight) = bounds
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
            val outputPath = file.absolutePath
            // 固定条带写锁：64 个锁对象按路径 hash 取模。替代按路径
            // computeIfAbsent 的无界锁表（永不清理，长批次下持续增长）；
            // 不同路径偶发碰撞只会短暂串行，内存有界。
            val pathLock = FILE_WRITE_LOCK_STRIPES[
                (outputPath.hashCode() % FILE_WRITE_LOCK_STRIPE_COUNT + FILE_WRITE_LOCK_STRIPE_COUNT) %
                    FILE_WRITE_LOCK_STRIPE_COUNT
            ]
            synchronized(pathLock) {
                val parent = file.parentFile
                if (parent != null) {
                    if (parent.exists() && !parent.isDirectory) {
                        throw IOException("Output parent is not a directory: ${parent.absolutePath}")
                    }
                    if (!parent.exists() && !parent.mkdirs()) {
                        throw IOException("Failed to create output directory: ${parent.absolutePath}")
                    }
                }

                if (file.exists() && file.isDirectory) {
                    logRepository.warning(
                        "NativeImageProcessingRepository",
                        "Output target is a directory, removing stale directory: $outputPath"
                    )
                    if (!file.deleteRecursively()) {
                        throw IOException("Output target is a directory and cannot be removed: $outputPath")
                    }
                }

                logOutputPathState(file = file, stage = "before_write")

                val tempFile = File(
                    parent,
                    "${file.name}.tmp-${System.nanoTime()}-${Thread.currentThread().id}"
                )

                try {
                    FileOutputStream(tempFile).use { output ->
                        output.write(encodedBytes)
                        output.fd.sync()
                    }

                    moveTempFileAtomically(tempFile = tempFile, outputFile = file)
                } catch (throwable: Throwable) {
                    if (!isExistsConflict(throwable)) {
                        throw throwable
                    }

                    logRepository.warning(
                        "NativeImageProcessingRepository",
                        "Output write conflict detected for $outputPath, attempting cleanup and retry: ${throwable.message}"
                    )
                    logOutputPathState(file = file, stage = "conflict_retry_before_cleanup")

                    if (file.exists()) {
                        if (!file.delete()) {
                            file.deleteRecursively()
                        }
                    }

                    moveTempFileAtomically(tempFile = tempFile, outputFile = file)
                } finally {
                    if (tempFile.exists()) {
                        tempFile.delete()
                    }
                    logOutputPathState(file = file, stage = "after_write")
                }
            }
        }

        private fun moveTempFileAtomically(
            tempFile: File,
            outputFile: File
        ) {
            val sourcePath = tempFile.toPath()
            val destinationPath = outputFile.toPath()
            try {
                Files.move(
                    sourcePath,
                    destinationPath,
                    StandardCopyOption.REPLACE_EXISTING,
                    StandardCopyOption.ATOMIC_MOVE
                )
            } catch (_: AtomicMoveNotSupportedException) {
                Files.move(
                    sourcePath,
                    destinationPath,
                    StandardCopyOption.REPLACE_EXISTING
                )
            }
        }

        private fun logOutputPathState(
            file: File,
            stage: String
        ) {
            val parent = file.parentFile
            val parentState = if (parent == null) {
                "parent=<none>"
            } else {
                "parent=${parent.absolutePath}, exists=${parent.exists()}, isDir=${parent.isDirectory}, canWrite=${parent.canWrite()}"
            }
            logRepository.info(
                "NativeImageProcessingRepository",
                "Output state[$stage]: path=${file.absolutePath}, exists=${file.exists()}, isFile=${file.isFile}, isDir=${file.isDirectory}, canWrite=${file.canWrite()}, $parentState"
            )
        }

    }

    companion object {
        private const val DEVICE_TYPE_CPU = 0
        private const val DEVICE_TYPE_GPU = 1
        private const val DEFAULT_SCALE_FACTOR = 2
        private const val VALIDATION_OUTPUT_FORMAT = "png"
        private const val DEFAULT_OUTPUT_QUALITY = 90
        private const val GPU_VALIDATION_INPUT_SIZE = 96
        private const val GPU_VALIDATION_MIN_DYNAMIC_RANGE = 24
        private const val GPU_VALIDATION_MIN_STD_DEV = 8.0
        private const val FILE_WRITE_LOCK_STRIPE_COUNT = 64
        private val FILE_WRITE_LOCK_STRIPES: Array<Any> = Array(FILE_WRITE_LOCK_STRIPE_COUNT) { Any() }
    }
}
