package com.crossscale.ui.components

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.withStyle
import com.crossscale.data.model.LogEntry
import com.crossscale.data.model.LogLevel
import com.crossscale.ui.theme.LogMonoStyle
import com.crossscale.ui.theme.Spacing
import java.time.ZoneId
import java.time.format.DateTimeFormatter

@Composable
fun LogViewer(logs: List<LogEntry>, modifier: Modifier = Modifier) {
    // Pattern parsing + zone lookup is non-trivial; hoist out of recomposition.
    val formatter = remember {
        DateTimeFormatter.ofPattern("HH:mm:ss").withZone(ZoneId.systemDefault())
    }

    LazyColumn(
        modifier = modifier.fillMaxSize(),
        verticalArrangement = Arrangement.spacedBy(Spacing.sm)
    ) {
        items(items = logs, key = { it.id }) { logEntry ->
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(vertical = Spacing.xs),
                verticalArrangement = Arrangement.spacedBy(Spacing.xs)
            ) {
                Text(
                    text = "[${formatter.format(logEntry.timestamp)}] [${logEntry.tag}]",
                    style = MaterialTheme.typography.labelMedium,
                    fontWeight = FontWeight.Medium
                )
                Text(
                    text = buildAnnotatedString {
                        // 仅级别标签着色，正文保持中性色，避免 E 级整行红底过重。
                        withStyle(SpanStyle(color = levelColor(logEntry.level))) {
                            append("[${logEntry.level}] ")
                        }
                        append(logEntry.message)
                    },
                    style = LogMonoStyle
                )
            }
        }
    }
}

@Composable
private fun levelColor(level: LogLevel): Color {
    val colorScheme = MaterialTheme.colorScheme
    return when (level) {
        LogLevel.DEBUG -> colorScheme.onSurfaceVariant
        LogLevel.INFO -> colorScheme.onSurfaceVariant
        LogLevel.WARNING -> colorScheme.onSurfaceVariant
        LogLevel.ERROR -> colorScheme.error
        LogLevel.CRITICAL -> colorScheme.error
    }
}
