package io.upix.data.repository

import android.content.Context
import android.content.SharedPreferences
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import androidx.core.content.edit
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * Minimal at-rest sealing for the pairing trust table, replacing the
 * deprecated androidx.security-crypto (EncryptedSharedPreferences).
 *
 * Values are sealed with a non-exportable Android Keystore AES-256-GCM key
 * and stored as Base64(iv || ciphertext) in a plain private SharedPreferences
 * file. A failed unseal (corrupt entry, alien plaintext from an older storage
 * scheme) reads as "absent": losing trust entries is safe — the user just
 * re-pairs, which re-establishes the trust anchor from scratch.
 *
 * Not internally synchronized; callers already confine access to IO threads.
 * The stored pairing records hold PUBLIC key material (peer identity keys and
 * fingerprints) — the sealing buys tamper-evidence and at-rest hygiene, not
 * secrecy of secrets.
 */
class KeystoreSecretStore(context: Context, private val prefsName: String) {

    private val appContext = context.applicationContext

    private val prefs: SharedPreferences by lazy {
        appContext.getSharedPreferences(prefsName, Context.MODE_PRIVATE)
    }

    private val key: SecretKey by lazy { getOrCreateKey() }

    fun put(name: String, value: String) {
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(Cipher.ENCRYPT_MODE, key)
        val sealed = cipher.iv + cipher.doFinal(value.toByteArray(Charsets.UTF_8))
        val encoded = Base64.encodeToString(sealed, Base64.NO_WRAP)
        prefs.edit { putString(name, encoded) }
    }

    fun get(name: String): String? {
        val encoded = prefs.getString(name, null) ?: return null
        return runCatching {
            val sealed = Base64.decode(encoded, Base64.NO_WRAP)
            if (sealed.size <= IV_SIZE_BYTES) return null
            val cipher = Cipher.getInstance(TRANSFORMATION)
            cipher.init(
                Cipher.DECRYPT_MODE, key,
                GCMParameterSpec(TAG_BITS, sealed, 0, IV_SIZE_BYTES)
            )
            val plain = cipher.doFinal(sealed, IV_SIZE_BYTES, sealed.size - IV_SIZE_BYTES)
            String(plain, Charsets.UTF_8)
        }.getOrNull()
    }

    private fun getOrCreateKey(): SecretKey {
        val keyStore = KeyStore.getInstance(ANDROID_KEYSTORE).apply { load(null) }
        (keyStore.getKey(KEY_ALIAS, null) as? SecretKey)?.let { return it }
        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, ANDROID_KEYSTORE)
        generator.init(
            KeyGenParameterSpec.Builder(
                KEY_ALIAS,
                KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT
            )
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build()
        )
        return generator.generateKey()
    }

    private companion object {
        const val ANDROID_KEYSTORE = "AndroidKeyStore"
        const val KEY_ALIAS = "upix_trust_v1"
        const val TRANSFORMATION = "AES/GCM/NoPadding"
        const val IV_SIZE_BYTES = 12
        const val TAG_BITS = 128
    }
}
