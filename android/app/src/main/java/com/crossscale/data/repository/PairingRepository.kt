package com.crossscale.data.repository

import android.content.Context

class PairingRepository(
    context: Context,
    private val logRepository: LogRepository
) {
    private val prefs = context.applicationContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    fun savePairing(host: String, port: Int, authKey: String) {
        val normalizedHost = host.trim()
        val normalizedKey = authKey.trim()
        if (normalizedHost.isBlank() || port !in 1..65535 || normalizedKey.isBlank()) {
            return
        }
        prefs.edit().putString(keyFor(normalizedHost, port), normalizedKey).apply()
        logRepository.info("PairingRepository", "Saved pairing for $normalizedHost:$port")
    }

    fun getAuthKey(host: String, port: Int): String? {
        val normalizedHost = host.trim()
        if (normalizedHost.isBlank() || port !in 1..65535) {
            return null
        }
        return prefs.getString(keyFor(normalizedHost, port), null)?.takeIf { it.isNotBlank() }
    }

    private fun keyFor(host: String, port: Int): String = "paired:$host:$port"

    private companion object {
        private const val PREFS_NAME = "crossscale_paired_devices"
    }
}
