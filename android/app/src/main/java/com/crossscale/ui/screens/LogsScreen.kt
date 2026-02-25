package com.crossscale.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import com.crossscale.data.model.LogEntry
import com.crossscale.data.model.LogLevel
import com.crossscale.ui.components.LogViewer
import com.crossscale.ui.theme.BorderlessTokens

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LogsScreen(
    logs: List<LogEntry>,
    onClearLogs: () -> Unit
) {
    var selectedLevel by remember { mutableStateOf<LogLevel?>(null) }
    var query by remember { mutableStateOf("") }
    val minLevel = selectedLevel

    val filteredLogs = remember(logs, minLevel, query) {
        logs
            .asSequence()
            .filter { entry ->
                minLevel == null || entry.level.ordinal >= minLevel.ordinal
            }
            .filter { entry ->
                if (query.isBlank()) {
                    true
                } else {
                    entry.tag.contains(query, ignoreCase = true) ||
                        entry.message.contains(query, ignoreCase = true)
                }
            }
            .sortedByDescending { it.id }
            .toList()
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(
                horizontal = BorderlessTokens.ScreenPaddingHorizontal,
                vertical = BorderlessTokens.ScreenPaddingVertical
            ),
        verticalArrangement = Arrangement.spacedBy(BorderlessTokens.SectionCompactSpacing)
    ) {
        Text(
            text = "日志",
            style = MaterialTheme.typography.titleMedium,
            fontWeight = BorderlessTokens.SectionTitleWeight
        )
        Text(
            text = "当前结果：${filteredLogs.size} 条",
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            style = MaterialTheme.typography.bodySmall
        )
        HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerStrongAlpha))

        OutlinedTextField(
            value = query,
            onValueChange = { query = it },
            modifier = Modifier.fillMaxWidth(),
            label = { Text("搜索") },
            singleLine = true
        )

        LazyRow(horizontalArrangement = Arrangement.spacedBy(BorderlessTokens.SectionCompactSpacing)) {
            item {
                FilterChip(
                    selected = selectedLevel == null,
                    onClick = { selectedLevel = null },
                    label = { Text("全部") }
                )
            }
            items(LogLevel.entries) { level ->
                FilterChip(
                    selected = selectedLevel == level,
                    onClick = { selectedLevel = level },
                    label = { Text(level.name) }
                )
            }
        }

        OutlinedButton(
            onClick = onClearLogs,
            modifier = Modifier
                .fillMaxWidth()
                .heightIn(min = BorderlessTokens.InteractiveRowMinHeight)
        ) {
            Text("清空日志")
        }

        HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerStrongAlpha))
        LogViewer(logs = filteredLogs)
    }
}
