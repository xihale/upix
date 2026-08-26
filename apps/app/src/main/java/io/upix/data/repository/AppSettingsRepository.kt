package com.upix.data.repository

import android.content.Context
import kotlinx.serialization.json.Json

/**
 * App 级设置的小仓储（SharedPreferences 持久化）。
 *
 * 数据流的加密由协议本身保证（所有帧都经 EncryptedSession 密封），
 * 没有可配置的开关。当前承载高级编码参数（EncoderSettings）。
 */
class AppSettingsRepository(context: Context) {
    private val prefs = context.applicationContext
        .getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    private val json = Json {
        encodeDefaults = true
        ignoreUnknownKeys = true
    }

    /** 读取持久化的编码参数；无记录或损坏时回退默认值（历史行为）。 */
    fun loadEncoderSettings(): EncoderSettings {
        val stored = prefs.getString(KEY_ENCODER_SETTINGS, null) ?: return EncoderSettings()
        return runCatching { json.decodeFromString<EncoderSettings>(stored) }
            .getOrDefault(EncoderSettings())
    }

    fun saveEncoderSettings(settings: EncoderSettings) {
        prefs.edit()
            .putString(KEY_ENCODER_SETTINGS, json.encodeToString(settings))
            .apply()
    }

    private companion object {
        const val PREFS_NAME = "upix_settings"
        const val KEY_ENCODER_SETTINGS = "encoder_settings"
    }
}
