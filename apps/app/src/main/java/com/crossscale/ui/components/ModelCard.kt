package com.crossscale.ui.components

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.CheckCircle
import androidx.compose.material.icons.outlined.Delete
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
import com.crossscale.R
import com.crossscale.data.model.ModelInfo
import com.crossscale.ui.theme.Spacing

/**
 * 模型卡：名称 + 规格单行 mono 小字 + 行尾操作 IconButton。
 * 列表内模型均为已导入（本地）模型；下载入口在屏级操作区。
 */
@Composable
fun ModelCard(
    model: ModelInfo,
    isSelected: Boolean,
    onSelect: (ModelInfo) -> Unit,
    onDelete: (ModelInfo) -> Unit
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
                    text = model.name,
                    style = MaterialTheme.typography.titleSmall,
                    maxLines = 1,
                    modifier = Modifier.weight(1f)
                )
                IconButton(onClick = { onSelect(model) }) {
                    Icon(
                        imageVector = Icons.Outlined.CheckCircle,
                        contentDescription = stringResource(
                            if (isSelected) R.string.models_selected else R.string.models_select
                        ),
                        tint = if (isSelected) {
                            MaterialTheme.colorScheme.primary
                        } else {
                            MaterialTheme.colorScheme.onSurfaceVariant
                        }
                    )
                }
                IconButton(onClick = { onDelete(model) }) {
                    Icon(
                        imageVector = Icons.Outlined.Delete,
                        contentDescription = stringResource(R.string.models_delete),
                        tint = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }

            Text(
                text = listOf(
                    stringResource(R.string.config_scale_value, model.scaleFactor),
                    stringResource(R.string.models_spec_tile, model.tileSize),
                    stringResource(R.string.models_spec_channels, model.inputChannels),
                    stringResource(
                        R.string.models_spec_size,
                        model.totalSize / 1024 / 1024
                    )
                ).joinToString(" · "),
                style = specTextStyle,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1
            )
        }
    }
}
