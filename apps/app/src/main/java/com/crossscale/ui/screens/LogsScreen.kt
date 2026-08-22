package com.crossscale.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.AnnotatedString
import com.crossscale.R
import com.crossscale.data.model.LogEntry
import com.crossscale.data.model.LogLevel
import com.crossscale.ui.components.LogViewer
import com.crossscale.ui.theme.Spacing

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
            .padding(horizontal = Spacing.xl)
            .padding(top = Spacing.md, bottom = Spacing.md),
        verticalArrangement = Arrangement.spacedBy(Spacing.md)
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Text(
                text = stringResource(R.string.common_logs),
                style = MaterialTheme.typography.titleLarge
            )
            OutlinedButton(onClick = onClearLogs) {
                Text(stringResource(R.string.logs_clear))
            }
            val clipboard = LocalClipboardManager.current
            OutlinedButton(onClick = {
                val text = filteredLogs.joinToString(separator = "\n") { entry ->
                    "${entry.level} [${entry.tag}] ${entry.message}"
                }
                clipboard.setText(AnnotatedString(text))
            }) {
                Text(stringResource(R.string.logs_copy))
            }
        }
        Text(
            text = stringResource(R.string.logs_result_count, filteredLogs.size),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            style = MaterialTheme.typography.bodySmall
        )

        OutlinedTextField(
            value = query,
            onValueChange = { query = it },
            modifier = Modifier.fillMaxWidth(),
            label = { Text(stringResource(R.string.logs_search_label)) },
            singleLine = true
        )

        LazyRow(horizontalArrangement = Arrangement.spacedBy(Spacing.sm)) {
            item {
                FilterChip(
                    selected = selectedLevel == null,
                    onClick = { selectedLevel = null },
                    label = { Text(stringResource(R.string.logs_filter_all)) }
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

        Surface(
            modifier = Modifier.fillMaxSize(),
            shape = MaterialTheme.shapes.extraSmall,
            color = MaterialTheme.colorScheme.surfaceContainerLow
        ) {
            LogViewer(
                logs = filteredLogs,
                modifier = Modifier.padding(Spacing.md)
            )
        }
    }
}
