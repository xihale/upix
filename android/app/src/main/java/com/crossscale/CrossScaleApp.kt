package com.crossscale

import android.app.Application

class CrossScaleApp : Application() {
    override fun onCreate() {
        super.onCreate()
        
        System.loadLibrary("crossscale-native")
    }
}
