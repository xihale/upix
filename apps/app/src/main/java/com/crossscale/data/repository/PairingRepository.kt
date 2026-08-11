package com.crossscale.data.repository

import android.content.Context
import android.content.SharedPreferences
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey
import org.json.JSONObject

data class TrustedPC(
    val identityPubKey: String,
    val fingerprint: String
)

class PairingRepository(
    context: Context,
    private val logRepository: LogRepository
) {
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

    fun saveTrustedPC(host: String, port: Int, pc: TrustedPC) {
        val normalizedHost = host.trim()
        if (normalizedHost.isBlank() || port !in 1..65535 || pc.identityPubKey.isBlank()) return
        val blob = JSONObject().apply {
            put("pk", pc.identityPubKey)
            put("fp", pc.fingerprint)
        }.toString()
        prefs.edit().putString(keyFor(normalizedHost, port), blob).apply()
        logRepository.info("PairingRepository", "Saved trusted PC $normalizedHost:$port (fp=${pc.fingerprint})")
    }

    fun getTrustedPC(host: String, port: Int): TrustedPC? {
        val normalizedHost = host.trim()
        if (normalizedHost.isBlank() || port !in 1..65535) return null
        val blob = prefs.getString(keyFor(normalizedHost, port), null) ?: return null
        return runCatching {
            val obj = JSONObject(blob)
            val pk = obj.optString("pk")
            if (pk.isBlank()) null else TrustedPC(identityPubKey = pk, fingerprint = obj.optString("fp"))
        }.getOrNull()
    }

    fun isPaired(host: String, port: Int): Boolean = getTrustedPC(host, port) != null

    private fun keyFor(host: String, port: Int): String = "paired:$host:$port"

    private companion object {
        private const val PREFS_NAME = "crossscale_paired_devices"
    }
}
