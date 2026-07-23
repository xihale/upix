package com.crossscale.data.repository

import android.content.Context
import android.content.SharedPreferences
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey

class PairingRepository(
    context: Context,
    private val logRepository: LogRepository
) {
    // Security fix: Use EncryptedSharedPreferences to protect auth keys at rest.
    // This encrypts the stored data using the Android Keystore-backed MasterKey.
    private val prefs: SharedPreferences = run {
        val masterKey = MasterKey.Builder(context.applicationContext)
            .setKeyScheme(MasterKey.KeyScheme.AES256_GCM)
            .build()
        EncryptedSharedPreferences.create(
            context.applicationContext,
            PREFS_NAME,
            masterKey,
            EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
            EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM
        )
    }

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
