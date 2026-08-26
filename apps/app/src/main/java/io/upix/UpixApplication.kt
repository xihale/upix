package io.upix

import android.app.Application
import android.util.Log

class UpixApplication : Application() {
    companion object {
        private const val TAG = "Upix"
    }

    lateinit var container: AppContainer
        private set

    override fun onCreate() {
        super.onCreate()
        container = AppContainer(this)
        // Load/create the long-term Ed25519 identity (stored app-private).
        // Must happen before any coordinator/worker connect attempt.
        val identityFile = java.io.File(filesDir, "identity")
        val identityLoaded = runCatching {
            io.upix.nativelib.NativeWorker.nativeLoadIdentity(identityFile.absolutePath)
        }.getOrDefault(false)
        if (!identityLoaded) {
            // 身份缺失意味着后续所有配对/握手都会失败，必须留下明确线索。
            val lastError = runCatching {
                io.upix.nativelib.NCNNEngine.nativeGetLastError()
            }.getOrNull()?.trim().orEmpty()
            Log.e(
                TAG,
                "Failed to load/create native identity at ${identityFile.absolutePath}" +
                    (lastError.takeIf { it.isNotBlank() }?.let { ": $it" } ?: "")
            )
        }
        Log.i(TAG, "Upix Application started")
    }
}
