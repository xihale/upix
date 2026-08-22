package com.crossscale.ui.components

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.CloudDownload
import androidx.compose.material.icons.outlined.CloudUpload
import androidx.compose.material.icons.outlined.Computer
import androidx.compose.material.icons.outlined.Delete
import androidx.compose.material.icons.outlined.Smartphone
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.crossscale.R
import com.crossscale.data.model.ModelLibraryEntry
import com.crossscale.ui.theme.Spacing

/**
 * 模型库卡：名称 + 双端存在徽标（手机/电脑）+ 规格单行 mono 小字 + 行尾操作。
 * 单端存在的模型显示上传/下载动作；双端都有则无动作，仅可删除本地副本。
 */
@Composable
fun ModelCard(
    entry: ModelLibraryEntry,
    uploading: Boolean,
    downloading: Boolean,
    onUploadToPc: () -> Unit,
    onDownloadFromPc: () -> Unit,
    onDelete: (() -> Unit)?
) {
    val specTextStyle = MaterialTheme.typography.labelSmall.copy(fontFamily = FontFamily.Monospace)

    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.medium,
        color = MaterialTheme.colorScheme.surfaceContainer
    ) {
        Column(
            modifier = Modifier.padding(
                start = Spacing.lg,
                end = Spacing.sm,
                top = Spacing.md,
                bottom = Spacing.md
            ),
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
                if (!entry.onPhone && entry.onPc) {
                    if (downloading) {
                        CircularProgressIndicator(modifier = Modifier.size(20.dp), strokeWidth = 2.dp)
                    } else {
                        IconButton(onClick = onDownloadFromPc, enabled = !busy) {
                            Icon(
                                imageVector = Icons.Outlined.CloudDownload,
                                contentDescription = stringResource(R.string.models_action_download),
                                tint = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                    }
                } else if (entry.onPhone && !entry.onPc) {
                    if (uploading) {
                        CircularProgressIndicator(modifier = Modifier.size(20.dp), strokeWidth = 2.dp)
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
                    IconButton(onClick = onDelete) {
                        Icon(
                            imageVector = Icons.Outlined.Delete,
                            contentDescription = stringResource(R.string.models_delete),
                            tint = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
            }

            // 规格行：x2 · 2MB 后跟双端存在图标（只画实际存在的端）。
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(Spacing.xs)
            ) {
                Text(
                    text = listOf(
                        stringResource(R.string.config_scale_value, entry.scaleFactor),
                        stringResource(
                            R.string.models_spec_size,
                            entry.totalSize / 1024 / 1024
                        )
                    ).joinToString(" · "),
                    style = specTextStyle,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1
                )
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

