package com.crossscale.ui.screens

import android.widget.Toast
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Close
import androidx.compose.material.icons.outlined.ContentCopy
import androidx.compose.material.icons.outlined.DeleteSweep
import androidx.compose.material.icons.outlined.Search
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
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
import androidx.compose.ui.unit.dp
import com.crossscale.R
import com.crossscale.data.model.LogEntry
import com.crossscale.data.model.LogLevel
import com.crossscale.ui.components.LogViewer
import com.crossscale.ui.theme.LocalScreenMargin
import com.crossscale.ui.theme.Spacing
import com.crossscale.ui.theme.ValueMono

/**
 * 日志屏：单列三段式——搜索框、筛选/工具行、日志面板。
 * 复制/清空收进筛选行右侧（幽灵 IconButton），不再单独占一行；
 * 结果数并入同行尾部弱化展示，减少垂直碎片。
 */
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

        // 筛选 + 工具同一行：级别 chips 可横向滚动，右侧复制/清空固定。
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(Spacing.sm)
        ) {
            LazyRow(
                modifier = Modifier.weight(1f),
                horizontalArrangement = Arrangement.spacedBy(Spacing.sm),
                verticalAlignment = Alignment.CenterVertically
            ) {
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
                        // 级别用原始英文枚举（开发工具美学），等宽字体与日志正文同族。
                        label = { Text(text = level.name, style = ValueMono) }
                    )
                }
            }

            IconButton(onClick = {
                val text = filteredLogs.joinToString(separator = "\n") { entry ->
                    "${entry.level} [${entry.tag}] ${entry.message}"
                }
                clipboard.setText(AnnotatedString(text))
                Toast.makeText(context, copiedCountMessage, Toast.LENGTH_SHORT).show()
            }) {
                Icon(
                    imageVector = Icons.Outlined.ContentCopy,
                    contentDescription = stringResource(R.string.logs_copy),
                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.size(20.dp)
                )
            }

            // 清空为破坏性动作：确认弹窗兜底（§9.3）。
            IconButton(onClick = { showClearConfirm = true }) {
                Icon(
                    imageVector = Icons.Outlined.DeleteSweep,
                    contentDescription = stringResource(R.string.logs_clear),
                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.size(20.dp)
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

/** 级别色点已移除；chip 标签直接用枚举 name。 */
