package com.upix.ui.components

import android.widget.Toast
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withStyle
import com.upix.R
import com.upix.data.model.LogEntry
import com.upix.data.model.LogLevel
import com.upix.ui.theme.LogMonoStyle
import com.upix.ui.theme.Spacing
import java.time.ZoneId
import java.time.format.DateTimeFormatter

/** 条目默认折叠行数：超过即省略，点击展开。 */
private const val CollapsedMessageLines = 2

@Composable
fun LogViewer(logs: List<LogEntry>, modifier: Modifier = Modifier) {
    if (logs.isEmpty()) {
        // 空态：居中弱化提示，不渲染列表骨架。
        Box(modifier = modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            Text(
                text = stringResource(R.string.logs_empty),
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
        return
    }
    // Pattern parsing + zone lookup is non-trivial; hoist out of recomposition.
    val formatter = remember {
        DateTimeFormatter.ofPattern("HH:mm:ss").withZone(ZoneId.systemDefault())
    }
    // 依赖句柄全部提到列表外，条目内只做纯文本拼接。
    val clipboard = LocalClipboardManager.current
    val context = LocalContext.current
    val haptic = LocalHapticFeedback.current
    val copiedMessage = stringResource(R.string.log_entry_copied)
    // 头部行与正文同用 LogMonoStyle（§2.1 唯一认可的日志等宽槽位）：同族字形网格，
    // 各条目头部彼此对齐、正文彼此对齐；层级差异只靠颜色弱化表达。

    LazyColumn(
        modifier = modifier.fillMaxSize(),
        verticalArrangement = Arrangement.spacedBy(Spacing.xs)
    ) {
        items(items = logs, key = { it.id }) { logEntry ->
            val fullText = "[${formatter.format(logEntry.timestamp)}] [${logEntry.tag}] [${logEntry.level}] ${logEntry.message}"
            // 长条目默认折为 2 行省略，点击展开全文；避免超长 URL 类消息把列表撑乱。
            var expanded by remember(logEntry.id) { mutableStateOf(false) }
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .combinedClickable(
                        onLongClick = {
                            // 长按复制单条完整日志：触感反馈 + Toast 确认。
                            haptic.performHapticFeedback(HapticFeedbackType.LongPress)
                            clipboard.setText(AnnotatedString(fullText))
                            Toast.makeText(context, copiedMessage, Toast.LENGTH_SHORT).show()
                        },
                        onClick = { expanded = !expanded }
                    )
                    .padding(vertical = Spacing.xs),
                verticalArrangement = Arrangement.spacedBy(Spacing.xs)
            ) {
                // 文本不可选中（内层不用 SelectionContainer）：否则其长按手势
                // 会吞掉 combinedClickable，长按复制永远不触发。
                Text(
                    // 标签行放不下时折两行，不省略（长 tag 不丢字）。
                    text = "[${formatter.format(logEntry.timestamp)}] [${logEntry.tag}]",
                    style = LogMonoStyle,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis
                )
                Text(
                    text = buildAnnotatedString {
                        // 仅级别标签着色，正文保持中性色，避免 E 级整行红底过重。
                        withStyle(SpanStyle(color = levelColor(logEntry.level))) {
                            append("[${logEntry.level}] ")
                        }
                        append(logEntry.message)
                    },
                    style = LogMonoStyle,
                    maxLines = if (expanded) Int.MAX_VALUE else CollapsedMessageLines,
                    overflow = if (expanded) TextOverflow.Clip else TextOverflow.Ellipsis
                )
            }
        }
    }
}

@Composable
private fun levelColor(level: LogLevel): Color {
    val colorScheme = MaterialTheme.colorScheme
    return when (level) {
        LogLevel.ERROR, LogLevel.CRITICAL -> colorScheme.error
        else -> colorScheme.onSurfaceVariant
    }
}
