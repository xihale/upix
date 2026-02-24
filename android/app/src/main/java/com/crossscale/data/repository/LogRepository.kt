package com.crossscale.data.repository

import com.crossscale.data.model.LogEntry
import com.crossscale.data.model.LogLevel
import java.time.Instant
import java.util.concurrent.atomic.AtomicLong
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update

class LogRepository {
    private val idGenerator = AtomicLong(1L)
    private val maxLogCount = 2000

    private val _logs = MutableStateFlow<List<LogEntry>>(emptyList())
    val logs: StateFlow<List<LogEntry>> = _logs.asStateFlow()

    fun log(level: LogLevel, tag: String, message: String) {
        val entry = LogEntry(
            id = idGenerator.getAndIncrement(),
            timestamp = Instant.now(),
            level = level,
            tag = tag,
            message = message
        )
        _logs.update { current ->
            val next = current + entry
            if (next.size <= maxLogCount) {
                next
            } else {
                next.takeLast(maxLogCount)
            }
        }
    }

    fun debug(tag: String, message: String) {
        log(LogLevel.DEBUG, tag, message)
    }

    fun info(tag: String, message: String) {
        log(LogLevel.INFO, tag, message)
    }

    fun warning(tag: String, message: String) {
        log(LogLevel.WARNING, tag, message)
    }

    fun error(tag: String, message: String) {
        log(LogLevel.ERROR, tag, message)
    }

    fun critical(tag: String, message: String) {
        log(LogLevel.CRITICAL, tag, message)
    }

    fun clear() {
        _logs.value = emptyList()
    }
}
