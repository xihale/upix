package io.upix.util

import android.content.Context
import android.content.res.Configuration
import android.os.LocaleList
import androidx.annotation.StringRes
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.os.LocaleListCompat
import io.upix.R
import java.util.Locale

/**
 * In-app language options backed by AndroidX per-app locales
 * (`AppCompatDelegate.setApplicationLocales` / Android 13+ `LocaleManager`).
 */
enum class AppLanguage(
    val languageTag: String?,
    @param:StringRes val labelRes: Int
) {
    SYSTEM(languageTag = null, labelRes = R.string.settings_language_follow_system),
    SIMPLIFIED_CHINESE(languageTag = "zh-CN", labelRes = R.string.settings_language_zh_cn),
    ENGLISH(languageTag = "en", labelRes = R.string.settings_language_en);

    companion object {
        fun fromLocaleList(locales: LocaleListCompat): AppLanguage {
            for (index in 0 until locales.size()) {
                val locale = locales[index] ?: continue
                when (locale.language.lowercase(Locale.ROOT)) {
                    "zh" -> {
                        val script = locale.script.lowercase(Locale.ROOT)
                        val country = locale.country.uppercase(Locale.ROOT)
                        val isSimplified = script == "hans" ||
                            (script != "hant" && (country == "CN" || country == "SG" || country.isEmpty()))
                        if (isSimplified) return SIMPLIFIED_CHINESE
                    }
                    "en" -> return ENGLISH
                }
            }
            return SYSTEM
        }
    }
}

object AppLocale {
    fun currentLanguage(): AppLanguage =
        AppLanguage.fromLocaleList(AppCompatDelegate.getApplicationLocales())

    fun setLanguage(language: AppLanguage) {
        val target = language.languageTag
            ?.let(LocaleListCompat::forLanguageTags)
            ?: LocaleListCompat.getEmptyLocaleList()
        if (AppCompatDelegate.getApplicationLocales() != target) {
            AppCompatDelegate.setApplicationLocales(target)
        }
    }

    /**
     * Returns a [Context] configured with the active per-app locale override.
     *
     * On API 26-32, `AppCompatDelegate` wraps `AppCompatActivity` contexts but not
     * `Application` or `Service` contexts, so background repositories and foreground
     * notifications must resolve strings through this helper.
     */
    fun localizedContext(context: Context): Context {
        val appLocales = AppCompatDelegate.getApplicationLocales()
        if (appLocales.isEmpty) return context
        val unwrapped = appLocales.unwrap() as? LocaleList
        val targetLocales = unwrapped ?: run {
            val array = Array(appLocales.size()) { index -> appLocales[index] }
                .filterNotNull()
                .toTypedArray()
            if (array.isEmpty()) return context
            LocaleList(*array)
        }
        val currentPrimary = context.resources.configuration.locales.let { list ->
            if (list.isEmpty) null else list[0]
        }
        if (currentPrimary == targetLocales[0]) {
            return context
        }
        val config = Configuration(context.resources.configuration)
        config.setLocales(targetLocales)
        return context.createConfigurationContext(config)
    }
}
