package io.upix.data.model

import androidx.compose.runtime.Immutable
import java.time.Instant

// Instant is not on Compose's known-stable list; the annotation keeps the
// whole entry (a pure immutable value) skippable when passed into composables.
@Immutable
data class LogEntry(
    val id: Long,
    val timestamp: Instant,
    val level: LogLevel,
    val tag: String,
    val message: String
)

enum class LogLevel {
    DEBUG,
    INFO,
    WARNING,
    ERROR,
    CRITICAL
}
