package com.crossscale.data.repository

import android.content.Context

/**
 * App 级设置的小仓储（SharedPreferences 持久化）。
 *
 * 数据流的加密由协议本身保证（所有帧都经 EncryptedSession 密封），
 * 没有可配置的开关。
 */
class AppSettingsRepository(context: Context) {
    private val prefs = context.applicationContext
        .getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    private companion object {
        const val PREFS_NAME = "crossscale_settings"
    }
}
