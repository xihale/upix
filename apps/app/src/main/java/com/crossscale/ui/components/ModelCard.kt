package com.crossscale.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Check
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
import androidx.compose.ui.unit.dp
import com.crossscale.R
import com.crossscale.data.model.ModelInfo
import com.crossscale.ui.theme.Spacing

/**
 * 模型卡：名称 + 规格 chips（mono labelSmall）+ 当前标记对勾徽章 + 行尾操作 IconButton。
 * 列表内模型均为已导入（本地）模型；下载入口在屏级操作区。
 */
@Composable
fun ModelCard(
    model: ModelInfo,
    isSelected: Boolean,
    onSelect: (ModelInfo) -> Unit,
    onDelete: (ModelInfo) -> Unit
) {
    val chipTextStyle = MaterialTheme.typography.labelSmall.copy(fontFamily = FontFamily.Monospace)

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
                Box(
                    modifier = Modifier
                        .size(20.dp)
                        .background(
                            color = if (isSelected) {
                                MaterialTheme.colorScheme.primary
                            } else {
                                MaterialTheme.colorScheme.surfaceVariant
                            },
                            shape = CircleShape
                        ),
                    contentAlignment = Alignment.Center
                ) {
                    if (isSelected) {
                        Icon(
                            imageVector = Icons.Outlined.Check,
                            contentDescription = stringResource(R.string.models_current),
                            tint = MaterialTheme.colorScheme.onPrimary,
                            modifier = Modifier.size(14.dp)
                        )
                    }
                }
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

            Row(
                horizontalArrangement = Arrangement.spacedBy(Spacing.sm),
                verticalAlignment = Alignment.CenterVertically
            ) {
                SpecChip(
                    text = stringResource(R.string.config_scale_value, model.scaleFactor),
                    textStyle = chipTextStyle
                )
                SpecChip(
                    text = stringResource(R.string.models_spec_tile, model.tileSize),
                    textStyle = chipTextStyle
                )
                SpecChip(
                    text = stringResource(
                        R.string.models_spec_channels,
                        model.inputChannels
                    ),
                    textStyle = chipTextStyle
                )
                SpecChip(
                    text = stringResource(
                        R.string.models_spec_size,
                        model.totalSize / 1024 / 1024
                    ),
                    textStyle = chipTextStyle
                )
            }
        }
    }
}

@Composable
private fun SpecChip(text: String, textStyle: androidx.compose.ui.text.TextStyle) {
    Surface(
        shape = MaterialTheme.shapes.extraSmall,
        color = MaterialTheme.colorScheme.surfaceVariant,
        contentColor = MaterialTheme.colorScheme.onSurfaceVariant
    ) {
        Text(
            text = text,
            style = textStyle,
            modifier = Modifier.padding(horizontal = Spacing.sm, vertical = 2.dp)
        )
    }
}
