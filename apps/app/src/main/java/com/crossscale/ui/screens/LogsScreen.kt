package com.crossscale.ui.screens

import android.widget.Toast
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Close
import androidx.compose.material.icons.outlined.ContentCopy
import androidx.compose.material.icons.outlined.DeleteSweep
import androidx.compose.material.icons.outlined.Search
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.crossscale.R
import com.crossscale.data.model.LogEntry
import com.crossscale.data.model.LogLevel
import com.crossscale.ui.components.LogViewer
import com.crossscale.ui.theme.LocalScreenMargin
import com.crossscale.ui.theme.Spacing

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LogsScreen(
    logs: List<LogEntry>,
    onClearLogs: () -> Unit
) {
    // 枚举不可直接 Bundle 序列化：存 ordinal（-1 = 全部），映射回枚举。
    var selectedLevelOrdinal by rememberSaveable { mutableStateOf(-1) }
    val selectedLevel = remember(selectedLevelOrdinal) {
        LogLevel.entries.getOrNull(selectedLevelOrdinal)
    }
    var query by rememberSaveable { mutableStateOf("") }
    // 清空日志不可恢复：先弹确认框再执行。
    var showClearConfirm by remember { mutableStateOf(false) }
    val minLevel = selectedLevel
    val clipboard = LocalClipboardManager.current
    val context = LocalContext.current

    // logs 本身按 id 升序：过滤后 asReversed 得到最新在前，免去整表排序。
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
            .toList()
            .asReversed()
    }
    val copiedCountMessage = stringResource(R.string.logs_copied_count, filteredLogs.size)

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(horizontal = LocalScreenMargin.current)
            .padding(top = Spacing.md, bottom = Spacing.md),
        verticalArrangement = Arrangement.spacedBy(Spacing.md)
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(Spacing.sm)
        ) {
            OutlinedButton(onClick = { showClearConfirm = true }) {
                Icon(
                    imageVector = Icons.Outlined.DeleteSweep,
                    contentDescription = null,
                    modifier = Modifier.padding(end = Spacing.xs).size(18.dp)
                )
                Text(stringResource(R.string.logs_clear))
            }
            OutlinedButton(onClick = {
                val text = filteredLogs.joinToString(separator = "\n") { entry ->
                    "${entry.level} [${entry.tag}] ${entry.message}"
                }
                clipboard.setText(AnnotatedString(text))
                Toast.makeText(context, copiedCountMessage, Toast.LENGTH_SHORT).show()
            }) {
                Icon(
                    imageVector = Icons.Outlined.ContentCopy,
                    contentDescription = null,
                    modifier = Modifier.padding(end = Spacing.xs).size(18.dp)
                )
                Text(stringResource(R.string.logs_copy))
            }
        }
        Text(
            text = stringResource(R.string.logs_result_count, filteredLogs.size),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            style = MaterialTheme.typography.labelMedium
        )

        OutlinedTextField(
            value = query,
            onValueChange = { query = it },
            modifier = Modifier.fillMaxWidth(),
            label = { Text(stringResource(R.string.logs_search_label)) },
            singleLine = true,
            leadingIcon = {
                Icon(
                    imageVector = Icons.Outlined.Search,
                    contentDescription = null
                )
            },
            trailingIcon = {
                // 仅在有输入时出现的一键清除。
                if (query.isNotEmpty()) {
                    IconButton(onClick = { query = "" }) {
                        Icon(
                            imageVector = Icons.Outlined.Close,
                            contentDescription = stringResource(R.string.logs_clear_search_cd)
                        )
                    }
                }
            }
        )

        LazyRow(horizontalArrangement = Arrangement.spacedBy(Spacing.sm)) {
            item {
                FilterChip(
                    selected = selectedLevel == null,
                    onClick = { selectedLevelOrdinal = -1 },
                    label = { Text(stringResource(R.string.logs_filter_all)) }
                )
            }
            items(LogLevel.entries) { level ->
                FilterChip(
                    selected = selectedLevel == level,
                    onClick = { selectedLevelOrdinal = level.ordinal },
                    leadingIcon = {
                        // 级别色点：E/C 红（error），其余级别中性（onSurfaceVariant）。
                        Box(
                            modifier = Modifier
                                .size(LevelDotSize)
                                .background(color = levelDotColor(level), shape = CircleShape)
                        )
                    },
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

    if (showClearConfirm) {
        AlertDialog(
            onDismissRequest = { showClearConfirm = false },
            title = { Text(stringResource(R.string.logs_clear_confirm_title)) },
            text = { Text(stringResource(R.string.logs_clear_confirm_body)) },
            confirmButton = {
                TextButton(
                    onClick = {
                        showClearConfirm = false
                        onClearLogs()
                    }
                ) {
                    Text(stringResource(R.string.common_confirm))
                }
            },
            dismissButton = {
                TextButton(onClick = { showClearConfirm = false }) {
                    Text(stringResource(R.string.common_cancel))
                }
            }
        )
    }
}

/** 级别色点直径：小到不喧宾夺主，大到颜色可辨。 */
private val LevelDotSize = 8.dp

/**
 * 筛选 chip 的级别色点语义：ERROR/CRITICAL -> error，其余级别（含 WARNING）
 * 中性（onSurfaceVariant），与 LogViewer.levelColor 保持一致。
 * WARNING 不单独配色：琥珀色槽位为保留槽位（D3、§11 禁止 app 代码引用）。
 */
@Composable
private fun levelDotColor(level: LogLevel): Color = when (level) {
    LogLevel.ERROR, LogLevel.CRITICAL -> MaterialTheme.colorScheme.error
    else -> MaterialTheme.colorScheme.onSurfaceVariant
}
