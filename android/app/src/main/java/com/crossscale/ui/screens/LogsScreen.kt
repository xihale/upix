package com.crossscale.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
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
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.crossscale.data.model.LogEntry
import com.crossscale.data.model.LogLevel
import com.crossscale.ui.components.LayeredSectionCard
import com.crossscale.ui.components.LogViewer

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LogsScreen(
    logs: List<LogEntry>,
    onClearLogs: () -> Unit
) {
    var selectedLevel by remember { mutableStateOf<LogLevel?>(null) }
    var query by remember { mutableStateOf("") }
    var filterHelpExpanded by remember { mutableStateOf(false) }
    val minLevel = selectedLevel

    val filteredLogs = logs
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

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        Text(
            text = "系统日志",
            style = MaterialTheme.typography.headlineSmall
        )
        Text(
            text = "先用级别和关键字快速筛选，必要时再展开查看使用说明。",
            style = MaterialTheme.typography.bodyMedium
        )

        LayeredSectionCard(
            title = "筛选与维护",
            summary = "当前结果：${filteredLogs.size} 条",
            guide = "建议先筛选 WARNING/ERROR 定位问题，再结合关键字缩小范围。",
            expanded = filterHelpExpanded,
            onToggleExpanded = { filterHelpExpanded = !filterHelpExpanded },
            actions = {
                OutlinedTextField(
                    value = query,
                    onValueChange = { query = it },
                    modifier = Modifier.fillMaxWidth(),
                    label = { Text("搜索") },
                    singleLine = true
                )

                LazyRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
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
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Text("清空日志")
                }
            },
            details = {
                Text(
                    text = "日志按时间倒序显示。筛选级别后会保留“同级及以上”日志，便于快速定位异常链路。",
                    style = MaterialTheme.typography.bodySmall
                )
            }
        )

        Text(
            text = "共 ${filteredLogs.size} 条",
            style = MaterialTheme.typography.bodySmall
        )
        LogViewer(logs = filteredLogs)
    }
}
