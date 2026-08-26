package io.upix.util

import androidx.annotation.StringRes

/**
 * Resolves user-visible strings from string resources on behalf of
 * ViewModel-layer classes that must not depend on a Context directly.
 *
 * Production wiring lives in AppContainer (backed by appContext.getString).
 * Unit tests inject a fake returning fixed English/placeholder values, so the
 * VM public API stays String-based and no test assertions depend on resources.
 */
fun interface StringProvider {
    fun getString(@StringRes resId: Int, vararg formatArgs: Any): String
}
