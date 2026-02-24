package com.crossscale.network

import android.content.Context
import android.os.Environment
import com.crossscale.data.repository.LogRepository
import com.crossscale.native.NativeWorker
import java.io.File

class CoordinatorClient(
    context: Context,
    private val logRepository: LogRepository
) {
    private val appContext = context.applicationContext
    private val modelsDirectory = appContext.getExternalFilesDir(Environment.DIRECTORY_DOCUMENTS)
        ?.let { directory -> File(directory, "models") }
        ?: File(appContext.filesDir, "models")

    private var worker: NativeWorker? = null

    fun connect(host: String, port: Int, authKey: String): Boolean {
        if (host.isBlank() || port <= 0 || authKey.isBlank()) {
            logRepository.error("CoordinatorClient", "Invalid connection params")
            return false
        }
        if (!modelsDirectory.exists() && !modelsDirectory.mkdirs()) {
            logRepository.error("CoordinatorClient", "Failed to create models directory: ${modelsDirectory.absolutePath}")
            return false
        }
        val activeWorker = worker ?: NativeWorker().also { worker = it }
        val connected = try {
            activeWorker.connect(
                host = host,
                port = port,
                authKey = authKey,
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
            logRepository.info("CoordinatorClient", "Connected to $host:$port")
        } else {
            val reason = runCatching { activeWorker.lastError() }
                .getOrNull()
                .orEmpty()
                .trim()
            if (reason.isNotBlank()) {
                logRepository.warning("CoordinatorClient", "Connect rejected by native worker: $reason")
            } else {
                logRepository.warning("CoordinatorClient", "Connect rejected by native worker")
            }
        }
        return connected
    }

    fun disconnect() {
        worker?.disconnect()
        logRepository.info("CoordinatorClient", "Disconnected")
    }

    fun release() {
        worker?.close()
        worker = null
    }
}
