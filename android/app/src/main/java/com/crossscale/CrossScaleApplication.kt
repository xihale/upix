package com.crossscale

import android.app.Application
import android.util.Log

class CrossScaleApplication : Application() {
    companion object {
        const val TAG = "CrossScale"
    }

    override fun onCreate() {
        super.onCreate()
        Log.i(TAG, "CrossScale Application started")
    }
}
