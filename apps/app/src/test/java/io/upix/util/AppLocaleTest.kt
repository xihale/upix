package io.upix.util

import android.content.Context
import android.content.res.Configuration
import android.os.LocaleList
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.os.LocaleListCompat
import io.upix.R
import io.upix.data.model.classifyModelScene
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import java.util.Locale

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28, 34])
class AppLocaleTest {
    private lateinit var appContext: Context

    @Before
    fun setUp() {
        appContext = RuntimeEnvironment.getApplication()
        AppCompatDelegate.setApplicationLocales(LocaleListCompat.getEmptyLocaleList())
    }

    @After
    fun tearDown() {
        AppCompatDelegate.setApplicationLocales(LocaleListCompat.getEmptyLocaleList())
    }

    @Test
    fun `fromLocaleList parses supported tags and ignores traditional chinese or other languages`() {
        assertEquals(AppLanguage.SYSTEM, AppLanguage.fromLocaleList(LocaleListCompat.getEmptyLocaleList()))

        assertEquals(AppLanguage.ENGLISH, AppLanguage.fromLocaleList(LocaleListCompat.forLanguageTags("en")))
        assertEquals(AppLanguage.ENGLISH, AppLanguage.fromLocaleList(LocaleListCompat.forLanguageTags("en-US")))
        assertEquals(AppLanguage.ENGLISH, AppLanguage.fromLocaleList(LocaleListCompat.forLanguageTags("en-GB")))

        assertEquals(AppLanguage.SIMPLIFIED_CHINESE, AppLanguage.fromLocaleList(LocaleListCompat.forLanguageTags("zh-CN")))
        assertEquals(AppLanguage.SIMPLIFIED_CHINESE, AppLanguage.fromLocaleList(LocaleListCompat.forLanguageTags("zh-Hans")))
        assertEquals(AppLanguage.SIMPLIFIED_CHINESE, AppLanguage.fromLocaleList(LocaleListCompat.forLanguageTags("zh-Hans-CN")))
        assertEquals(AppLanguage.SIMPLIFIED_CHINESE, AppLanguage.fromLocaleList(LocaleListCompat.forLanguageTags("zh-SG")))
        assertEquals(AppLanguage.SIMPLIFIED_CHINESE, AppLanguage.fromLocaleList(LocaleListCompat.forLanguageTags("zh")))

        assertEquals(AppLanguage.SYSTEM, AppLanguage.fromLocaleList(LocaleListCompat.forLanguageTags("zh-TW")))
        assertEquals(AppLanguage.SYSTEM, AppLanguage.fromLocaleList(LocaleListCompat.forLanguageTags("zh-Hant-TW")))
        assertEquals(AppLanguage.SYSTEM, AppLanguage.fromLocaleList(LocaleListCompat.forLanguageTags("zh-HK")))
        assertEquals(AppLanguage.SYSTEM, AppLanguage.fromLocaleList(LocaleListCompat.forLanguageTags("fr-FR")))
    }

    @Test
    fun `strings resolve to English base Simplified Chinese for zh locales and fallback for unsupported locales`() {
        val enCtx = contextForTag("en")
        assertEquals("Cancel", enCtx.getString(R.string.common_cancel))
        assertEquals("Follow system", enCtx.getString(R.string.settings_language_follow_system))
        assertEquals("简体中文", enCtx.getString(R.string.settings_language_zh_cn))

        val zhCnCtx = contextForTag("zh-CN")
        assertEquals("取消", zhCnCtx.getString(R.string.common_cancel))
        assertEquals("跟随系统", zhCnCtx.getString(R.string.settings_language_follow_system))
        assertEquals("English", zhCnCtx.getString(R.string.settings_language_en))

        val zhHansCtx = contextForTag("zh-Hans")
        assertEquals("取消", zhHansCtx.getString(R.string.common_cancel))

        val zhHansCnCtx = contextForTag("zh-Hans-CN")
        assertEquals("取消", zhHansCnCtx.getString(R.string.common_cancel))

        val zhTwCtx = contextForTag("zh-TW")
        assertEquals("Cancel", zhTwCtx.getString(R.string.common_cancel))

        val frCtx = contextForTag("fr-FR")
        assertEquals("Cancel", frCtx.getString(R.string.common_cancel))
    }

    @Test
    fun `setLanguage and localizedContext switch active application locale dynamically`() {
        // AppCompatActivity registers an AppCompatDelegate (required on API 33+ for
        // AppCompatDelegate.setApplicationLocales) and also verifies Theme.Upix inherits Theme.AppCompat.
        val controller = Robolectric.buildActivity(AppCompatActivity::class.java).create()
        try {
            AppLocale.setLanguage(AppLanguage.SIMPLIFIED_CHINESE)
            assertEquals(AppLanguage.SIMPLIFIED_CHINESE, AppLocale.currentLanguage())
            assertEquals("取消", AppLocale.localizedContext(appContext).getString(R.string.common_cancel))

            AppLocale.setLanguage(AppLanguage.ENGLISH)
            assertEquals(AppLanguage.ENGLISH, AppLocale.currentLanguage())
            assertEquals("Cancel", AppLocale.localizedContext(appContext).getString(R.string.common_cancel))

            AppLocale.setLanguage(AppLanguage.SYSTEM)
            assertEquals(AppLanguage.SYSTEM, AppLocale.currentLanguage())
        } finally {
            controller.destroy()
        }
    }

    @Test
    fun `model scene labels resolve cleanly in both English and Simplified Chinese`() {
        val enCtx = contextForTag("en")
        val zhCtx = contextForTag("zh-CN")
        val samples = listOf(
            "real-esrganv3-anime" to ("Anime" to "动漫"),
            "real-esrganv3-general" to ("Photo" to "照片"),
            "dejpg" to ("Photo · Restore" to "照片·修复"),
            "deh264" to ("Video · Restore" to "视频·修复"),
            "archivist-medium" to ("Anime · Restore" to "动漫·修复"),
            "nes-composite" to ("Game · Restore" to "游戏·修复"),
            "ultrasharp" to ("General" to "综合")
        )
        samples.forEach { (modelName, expected) ->
            val scene = classifyModelScene(modelName)
            assertNotNull("Expected scene for $modelName", scene)
            assertEquals(expected.first, enCtx.getString(scene!!.labelRes))
            assertEquals(expected.second, zhCtx.getString(scene.labelRes))
        }
    }

    private fun contextForTag(tag: String): Context {
        val locale = Locale.forLanguageTag(tag)
        val config = Configuration(appContext.resources.configuration)
        config.setLocales(LocaleList(locale))
        return appContext.createConfigurationContext(config)
    }
}
