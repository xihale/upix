package com.crossscale

import android.app.Application
import android.util.Log

class CrossScaleApplication : Application() {
    companion object {
        private const val TAG = "CrossScale"
    }

    lateinit var container: AppContainer
        private set

    override fun onCreate() {
        super.onCreate()
        container = AppContainer(this)
        // Load/create the long-term Ed25519 identity (stored app-private).
        // Must happen before any coordinator/worker connect attempt.
        val identityFile = java.io.File(filesDir, "identity")
        com.crossscale.native.NativeWorker().loadIdentity(identityFile.absolutePath)
        Log.i(TAG, "CrossScale Application started")
    }
}
