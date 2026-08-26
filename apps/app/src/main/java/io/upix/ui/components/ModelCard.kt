package io.upix.ui.components

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.CloudDownload
import androidx.compose.material.icons.outlined.CloudUpload
import androidx.compose.material.icons.outlined.Computer
import androidx.compose.material.icons.outlined.Delete
import androidx.compose.material.icons.outlined.Smartphone
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import io.upix.R
import io.upix.data.model.ModelLibraryEntry
import io.upix.ui.theme.Spacing
import io.upix.ui.theme.valueStyleFor

/**
 * 模型库卡：名称 + 双端存在徽标（手机/电脑）+ 规格行走 §2.1 机值槽（`valueStyleFor` →
 * `ValueMono`，titleSmall 14/20 mono）+ 行尾操作。
 * 单端存在的模型显示上传/下载动作；双端都有则无动作，仅可删除本地副本。
 * 未连接 PC（[pcConnected] = false）时双端徽标与传输动作均无意义，一并隐藏。
 *
 * @param pcConnected 当前是否已连接 PC（决定 PC 相关图标与动作是否展示）。
 */
@Composable
fun ModelCard(
    entry: ModelLibraryEntry,
    uploading: Boolean,
    downloading: Boolean,
    pcConnected: Boolean,
    onUploadToPc: () -> Unit,
    onDownloadFromPc: () -> Unit,
    onDelete: (() -> Unit)?
) {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.medium,
        color = MaterialTheme.colorScheme.surfaceContainer
    ) {
        Column(
            // §9.2 卡片配方：统一 Spacing.lg 内距（行尾 IconButton 自带 48dp 触控盒，无需让位内缩）。
            modifier = Modifier.padding(Spacing.lg),
            verticalArrangement = Arrangement.spacedBy(Spacing.sm)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(Spacing.sm)
            ) {
                Text(
                    text = entry.name,
                    style = MaterialTheme.typography.titleSmall,
                    maxLines = 1,
                    modifier = Modifier.weight(1f)
                )

                val busy = uploading || downloading
                if (!entry.onPhone && entry.onPc && pcConnected) {
                    if (downloading) {
                        BusyIndicator()
                    } else {
                        IconButton(onClick = onDownloadFromPc, enabled = !busy) {
                            Icon(
                                imageVector = Icons.Outlined.CloudDownload,
                                contentDescription = stringResource(R.string.models_action_download),
                                tint = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                    }
                } else if (entry.onPhone && !entry.onPc && pcConnected) {
                    if (uploading) {
                        BusyIndicator()
                    } else {
                        IconButton(onClick = onUploadToPc, enabled = !busy) {
                            Icon(
                                imageVector = Icons.Outlined.CloudUpload,
                                contentDescription = stringResource(R.string.models_action_upload),
                                tint = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                    }
                }

                if (onDelete != null) {
                    // 上传/下载进行中禁止删除，避免传输中删掉本地文件。
                    IconButton(onClick = onDelete, enabled = !busy) {
                        Icon(
                            imageVector = Icons.Outlined.Delete,
                            contentDescription = stringResource(R.string.models_delete),
                            tint = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
            }

            // 规格行：x2 · 2MB 后跟双端存在图标（只画实际存在的端，且仅在已
            // 连接 PC 时展示——离线时 PC 标记无信息量）。规格文本与徽标组之
            // 间用 Spacing.sm 呼吸间隔，徽标组内部 xs，避免 14dp 图标贴字。
            // 等宽判定按原子值进行，分隔符「·」为装饰性中性字符不参与判定
            // （docs/DESIGN_LANGUAGE.md §2.2 修订）。
            val showDeviceBadges = pcConnected && (entry.onPhone || entry.onPc)
            val scaleText = stringResource(R.string.config_scale_value, entry.scaleFactor)
            val sizeText = formatSizeBytes(entry.totalSize)
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(Spacing.xs)
            ) {
                Text(
                    text = listOf(scaleText, sizeText).joinToString(" · "),
                    style = valueStyleFor(scaleText, sizeText),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1
                )
                if (showDeviceBadges) {
                    Row(
                        modifier = Modifier.padding(start = Spacing.sm),
                        horizontalArrangement = Arrangement.spacedBy(Spacing.xs),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        if (entry.onPhone) {
                            Icon(
                                imageVector = Icons.Outlined.Smartphone,
                                contentDescription = null,
                                modifier = Modifier.size(14.dp),
                                tint = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                        if (entry.onPc) {
                            Icon(
                                imageVector = Icons.Outlined.Computer,
                                contentDescription = null,
                                modifier = Modifier.size(14.dp),
                                tint = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                    }
                }
            }
        }
    }
}

