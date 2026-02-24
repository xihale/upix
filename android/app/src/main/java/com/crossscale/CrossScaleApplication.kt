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
        Log.i(TAG, "CrossScale Application started")
    }
}
