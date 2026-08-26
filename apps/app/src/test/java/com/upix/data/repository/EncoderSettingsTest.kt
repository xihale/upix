package com.upix.data.repository

import kotlinx.serialization.json.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment

/**
 * EncoderSettings 持久化与 native JSON 契约：
 *  - SharedPreferences 往返（含损坏回退默认）
 *  - JSON 字段名与 native upix::EncoderOptions 对齐（snake_case）
 */
@RunWith(RobolectricTestRunner::class)
class EncoderSettingsTest {

    private fun repository(): AppSettingsRepository {
        val context = RuntimeEnvironment.getApplication()
        context.getSharedPreferences("upix_settings", 0).edit().clear().commit()
        return AppSettingsRepository(context)
    }

    @Test
    fun `defaults match historical behavior`() {
        val settings = EncoderSettings()
        assertEquals(90, settings.quality)
        assertEquals(-1, settings.pngLevel)
        assertEquals(420, settings.jpegSubsampling)
        assertEquals(-1, settings.webpMethod)
        assertFalse(settings.webpLossless)
        assertEquals(6, settings.avifSpeed)
        assertEquals(420, settings.avifSubsampling)
        assertEquals(0, settings.avifThreads)
    }

    @Test
    fun `persistence round-trips every field`() {
        val repo = repository()
        val settings = EncoderSettings(
            quality = 75,
            pngLevel = 7,
            jpegSubsampling = 444,
            webpMethod = 5,
            webpLossless = true,
            avifSpeed = 9,
            avifSubsampling = 444,
            avifThreads = 12
        )
        repo.saveEncoderSettings(settings)
        assertEquals(settings, repo.loadEncoderSettings())
    }

    @Test
    fun `missing repository entry falls back to defaults`() {
        assertEquals(EncoderSettings(), repository().loadEncoderSettings())
    }

    @Test
    fun `corrupted persisted entry falls back to defaults`() {
        val repo = repository()
        val context = RuntimeEnvironment.getApplication()
        context.getSharedPreferences("upix_settings", 0)
            .edit().putString("encoder_settings", "not json {").commit()
        assertEquals(EncoderSettings(), repo.loadEncoderSettings())
    }

    @Test
    fun `native json uses snake_case keys matching the native struct`() {
        val json = EncoderSettings(
            quality = 85,
            pngLevel = 3,
            jpegSubsampling = 422,
            webpMethod = 2,
            webpLossless = true,
            avifSpeed = 8,
            avifSubsampling = 444,
            avifThreads = 4
        ).toNativeJson()
        // glaze 反射按 C++ 成员名（snake_case）解析；键名不同会被静默忽略，
        // 参数就丢了 —— 这里逐键钉死契约。
        assertTrue(json.contains("\"png_level\":3"))
        assertTrue(json.contains("\"jpeg_subsampling\":422"))
        assertTrue(json.contains("\"webp_method\":2"))
        assertTrue(json.contains("\"webp_lossless\":true"))
        assertTrue(json.contains("\"avif_speed\":8"))
        assertTrue(json.contains("\"avif_subsampling\":444"))
        assertTrue(json.contains("\"avif_threads\":4"))
    }

    @Test
    fun `native json round-trips through the shared serializer`() {
        val settings = EncoderSettings(quality = 66, pngLevel = 5, avifSpeed = 10)
        val parsed = Json { ignoreUnknownKeys = true }
            .decodeFromString<EncoderSettings>(settings.toNativeJson())
        assertEquals(settings, parsed)
    }

    @Test
    fun `quality is clamped into 1-100`() {
        assertEquals(1, EncoderSettings(quality = -7).normalizedQuality)
        assertEquals(100, EncoderSettings(quality = 250).normalizedQuality)
        assertEquals(90, EncoderSettings().normalizedQuality)
    }
}
