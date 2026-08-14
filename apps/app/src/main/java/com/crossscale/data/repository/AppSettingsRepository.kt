package com.crossscale.data.repository

import android.content.Context

/**
 * App 级设置的小仓储（SharedPreferences 持久化）。
 *
 * 注意：DeviceViewModel 在建立连接时也会读取同一个 stream_encrypted 键
 * （只读、不写），键名保持一致。
 */
class AppSettingsRepository(context: Context) {
    private val prefs = context.applicationContext
        .getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    /** 加密数据流开关（默认关闭 = 吞吐优先）。 */
    fun isStreamEncrypted(): Boolean = prefs.getBoolean(STREAM_ENCRYPTED_KEY, false)

    fun setStreamEncrypted(enabled: Boolean) {
        prefs.edit()
            .putBoolean(STREAM_ENCRYPTED_KEY, enabled)
            .apply()
    }

    private companion object {
        const val PREFS_NAME = "crossscale_settings"
        const val STREAM_ENCRYPTED_KEY = "stream_encrypted"
    }
}
