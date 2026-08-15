package com.crossscale.ui.components

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import com.crossscale.data.model.LogEntry
import com.crossscale.data.model.LogLevel
import com.crossscale.ui.theme.BorderlessTokens
import com.crossscale.ui.theme.LogMonoStyle
import java.time.ZoneId
import java.time.format.DateTimeFormatter

@Composable
fun LogViewer(logs: List<LogEntry>) {
    // Pattern parsing + zone lookup is non-trivial; hoist out of recomposition.
    val formatter = remember {
        DateTimeFormatter.ofPattern("HH:mm:ss").withZone(ZoneId.systemDefault())
    }

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        verticalArrangement = Arrangement.spacedBy(BorderlessTokens.ListItemSpacing)
    ) {
        items(items = logs, key = { it.id }) { logEntry ->
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(vertical = BorderlessTokens.SectionCompactSpacing),
                verticalArrangement = Arrangement.spacedBy(BorderlessTokens.SectionCompactSpacing / 2)
            ) {
                Text(
                    text = "[${formatter.format(logEntry.timestamp)}] [${logEntry.tag}]",
                    style = MaterialTheme.typography.labelMedium,
                    fontWeight = FontWeight.Medium
                )
                Text(
                    text = "[${logEntry.level}] ${logEntry.message}",
                    color = levelColor(logEntry.level),
                    style = LogMonoStyle
                )
                HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerSubtleAlpha))
            }
        }
    }
}

@Composable
private fun levelColor(level: LogLevel): Color {
    val colorScheme = MaterialTheme.colorScheme
    return when (level) {
        LogLevel.DEBUG -> colorScheme.onSurfaceVariant
        LogLevel.INFO -> colorScheme.primary
        LogLevel.WARNING -> colorScheme.tertiary
        LogLevel.ERROR -> colorScheme.error
        LogLevel.CRITICAL -> colorScheme.error
    }
}
