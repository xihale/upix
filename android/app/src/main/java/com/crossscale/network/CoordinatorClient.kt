package com.crossscale.network

import com.crossscale.data.repository.LogRepository
import com.crossscale.native.NativeWorker

class CoordinatorClient(
    private val logRepository: LogRepository
) {
    private var worker: NativeWorker? = null

    fun connect(host: String, port: Int, authKey: String): Boolean {
        if (host.isBlank() || port <= 0 || authKey.isBlank()) {
            logRepository.error("CoordinatorClient", "Invalid connection params")
            return false
        }
        val activeWorker = worker ?: NativeWorker().also { worker = it }
        val connected = try {
            activeWorker.connect(host = host, port = port, authKey = authKey)
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
            logRepository.warning("CoordinatorClient", "Connect rejected by native worker")
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
