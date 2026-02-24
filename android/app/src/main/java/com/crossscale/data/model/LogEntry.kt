package com.crossscale.data.model

import java.time.Instant

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
