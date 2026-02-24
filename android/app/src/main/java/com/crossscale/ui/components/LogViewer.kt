package com.crossscale.ui.components

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.crossscale.data.model.LogEntry
import com.crossscale.data.model.LogLevel
import java.time.ZoneId
import java.time.format.DateTimeFormatter

@Composable
fun LogViewer(logs: List<LogEntry>) {
    val formatter = DateTimeFormatter.ofPattern("HH:mm:ss").withZone(ZoneId.systemDefault())

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        verticalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        items(items = logs, key = { it.id }) { logEntry ->
            Card(modifier = Modifier.fillMaxWidth()) {
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(12.dp),
                    verticalArrangement = Arrangement.spacedBy(4.dp)
                ) {
                    Text(
                        text = "[${formatter.format(logEntry.timestamp)}] [${logEntry.tag}]",
                        style = MaterialTheme.typography.labelMedium
                    )
                    Text(
                        text = "[${logEntry.level}] ${logEntry.message}",
                        color = levelColor(logEntry.level),
                        style = MaterialTheme.typography.bodySmall
                    )
                }
            }
        }
    }
}

private fun levelColor(level: LogLevel): Color {
    return when (level) {
        LogLevel.DEBUG -> Color(0xFF6B7280)
        LogLevel.INFO -> Color(0xFF1D4ED8)
        LogLevel.WARNING -> Color(0xFFD97706)
        LogLevel.ERROR -> Color(0xFFB91C1C)
        LogLevel.CRITICAL -> Color(0xFF7F1D1D)
    }
}
