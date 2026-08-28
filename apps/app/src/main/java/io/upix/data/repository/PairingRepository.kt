package io.upix.data.repository

import android.content.Context
import android.content.SharedPreferences
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import androidx.core.content.edit

data class TrustedPC(
    val identityPubKey: String,
    val fingerprint: String
)

/**
 * The single PC the phone last paired with, used for app-start auto-reconnect.
 * [identityPubKey] drives the strict MITM check; [fingerprint] is for display.
 */
data class LastPairedPC(
    val host: String,
    val port: Int,
    val identityPubKey: String,
    val fingerprint: String?
)

class PairingRepository(
    context: Context,
    private val logRepository: LogRepository,
    // 可注入 IO 调度器（单测注入 TestDispatcher）
    private val ioDispatcher: CoroutineDispatcher = Dispatchers.IO
) {
    private val appContext = context.applicationContext

    // 懒初始化：MasterKey + EncryptedSharedPreferences 的密钥派生很重，
    // 不能在 Application.onCreate（主线程）里构建；首次访问发生在下面的
    // suspend 访问器内，全部切换到 IO 线程。
    private val prefs: SharedPreferences by lazy {
        val masterKey = MasterKey.Builder(appContext)
            .setKeyScheme(MasterKey.KeyScheme.AES256_GCM)
            .build()
        EncryptedSharedPreferences.create(
            appContext,
            PREFS_NAME,
            masterKey,
            EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
            EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM
        )
    }

    suspend fun saveTrustedPC(host: String, port: Int, pc: TrustedPC) = withContext(ioDispatcher) {
        val normalizedHost = host.trim()
        if (normalizedHost.isBlank() || port !in 1..65535 || pc.identityPubKey.isBlank()) {
            return@withContext
        }
        val blob = JSONObject().apply {
            put("pk", pc.identityPubKey)
            put("fp", pc.fingerprint)
        }.toString()
        prefs.edit { putString(keyFor(normalizedHost, port), blob) }
        logRepository.info("PairingRepository", "Saved trusted PC $normalizedHost:$port (fp=${pc.fingerprint})")
    }

    suspend fun getTrustedPC(host: String, port: Int): TrustedPC? = withContext(ioDispatcher) {
        val normalizedHost = host.trim()
        if (normalizedHost.isBlank() || port !in 1..65535) {
            return@withContext null
        }
        val blob = prefs.getString(keyFor(normalizedHost, port), null) ?: return@withContext null
        runCatching {
            val obj = JSONObject(blob)
            val pk = obj.optString("pk")
            if (pk.isBlank()) null else TrustedPC(identityPubKey = pk, fingerprint = obj.optString("fp"))
        }.getOrNull()
    }

    /**
     * Persist the single "last paired PC" record used for next-launch
     * auto-reconnect. Overwrites any previous entry. Kept in sync with the
     * per-host `paired:$host:$port` trust table by the caller on connect.
     */
    suspend fun saveLastPairedPC(
        host: String,
        port: Int,
        identityPubKey: String,
        fingerprint: String?
    ) = withContext(ioDispatcher) {
        val normalizedHost = host.trim()
        if (normalizedHost.isBlank() || port !in 1..65535 || identityPubKey.isBlank()) {
            return@withContext
        }
        val blob = JSONObject().apply {
            put("host", normalizedHost)
            put("port", port)
            put("pk", identityPubKey)
            put("fp", fingerprint ?: "")
        }.toString()
        prefs.edit { putString(KEY_LAST_PAIRED_PC, blob) }
        logRepository.info(
            "PairingRepository",
            "Saved last paired PC $normalizedHost:$port (fp=$fingerprint)"
        )
    }

    suspend fun getLastPairedPC(): LastPairedPC? = withContext(ioDispatcher) {
        val blob = prefs.getString(KEY_LAST_PAIRED_PC, null) ?: return@withContext null
        runCatching {
            val obj = JSONObject(blob)
            val host = obj.optString("host").trim()
            val port = obj.optInt("port", -1)
            val pk = obj.optString("pk").trim()
            if (host.isBlank() || port !in 1..65535 || pk.isBlank()) return@runCatching null
            val fp = obj.optString("fp").trim().ifBlank { null }
            LastPairedPC(host = host, port = port, identityPubKey = pk, fingerprint = fp)
        }.getOrNull()
    }

    private fun keyFor(host: String, port: Int): String = "paired:$host:$port"

    private companion object {
        private const val PREFS_NAME = "upix_paired_devices"
        private const val KEY_LAST_PAIRED_PC = "last_paired_pc"
    }
}
