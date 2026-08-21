package com.crossscale.ui.screens.home

import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.animateContentSize
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.toggleable
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Backup
import androidx.compose.material.icons.outlined.ChevronRight
import androidx.compose.material.icons.outlined.ExpandMore
import androidx.compose.material.icons.outlined.FolderOpen
import androidx.compose.material.icons.outlined.Image
import androidx.compose.material.icons.outlined.Memory
import androidx.compose.material.icons.outlined.Save
import androidx.compose.material.icons.outlined.SubdirectoryArrowRight
import androidx.compose.material.icons.outlined.Tune
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.crossscale.R
import com.crossscale.data.model.ModelInfo
import com.crossscale.ui.theme.EnterMotionSpec
import com.crossscale.ui.theme.ExpandSizeSpec
import com.crossscale.ui.theme.Spacing
import com.crossscale.ui.theme.ValueMono
import com.crossscale.ui.theme.pressable
import com.crossscale.ui.theme.valueStyleFor

/**
 * 参数卡：一张容器内聚合所有任务配置。
 * 行组件全部使用 M3 官方 [ListItem]（正确的字阶/高度/内边距），
 * 倍率用官方 [SingleChoiceSegmentedButtonRow] 直接切换（不再经 sheet）；
 * 开关行整行 toggleable + Switch 仅作指示（M3 官方模式）。
 */
@Composable
internal fun ParameterCard(
    selectedModel: ModelInfo?,
    notSelectedLabel: String,
    outputDirectoryLabel: String,
    overwriteOriginal: Boolean,
    backupOriginal: Boolean,
    processNestedFolders: Boolean,
    outputFormatLabel: String = "",
    availableScales: List<Int> = emptyList(),
    hasCustomOutputDirectory: Boolean = false,
    onPickModel: () -> Unit,
    onPickScale: (Int) -> Unit = {},
    onPickFormat: () -> Unit = {},
    onSelectOutputFolder: () -> Unit,
    onUseDefaultOutputFolder: () -> Unit,
    onToggleOverwriteOriginal: (Boolean) -> Unit,
    onToggleBackupOriginal: (Boolean) -> Unit,
    onToggleProcessNestedFolders: (Boolean) -> Unit
) {
    var advancedExpanded by remember { mutableStateOf(false) }
    val expandIconRotation by animateFloatAsState(
        targetValue = if (advancedExpanded) 180f else 0f,
        label = "advancedExpandRotation"
    )
    val transparent = ListItemDefaults.colors(containerColor = Color.Transparent)

    Surface(
        modifier = Modifier
            .fillMaxWidth()
            // 高级设置折叠区展开（250ms ease-out，统一 spec）。
            .animateContentSize(ExpandSizeSpec),
        shape = MaterialTheme.shapes.large,
        color = MaterialTheme.colorScheme.surfaceContainer
    ) {
        Column {
            // 模型 + 倍率是一个联动整体：倍率分段紧贴模型名下方（无独立标题块），
            // 点倍率即换模型，模型名用统一纯 fade 过渡，联动不突兀。
            // 点击区域只覆盖名称行；倍率分段独立响应，不触发打开模型 sheet。
            Column(modifier = Modifier.fillMaxWidth()) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .pressable(onClick = onPickModel)
                        .padding(horizontal = Spacing.lg, vertical = Spacing.md),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Icon(
                        imageVector = Icons.Outlined.Memory,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                    Column(
                        modifier = Modifier
                            .weight(1f)
                            .padding(start = Spacing.md),
                        verticalArrangement = Arrangement.spacedBy(1.dp)
                    ) {
                        Text(
                            text = stringResource(R.string.home_model_setting_label),
                            style = MaterialTheme.typography.labelMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                        val modelLabel = selectedModel?.name ?: notSelectedLabel
                        AnimatedContent(
                            targetState = modelLabel,
                            transitionSpec = {
                                fadeIn(EnterMotionSpec) togetherWith fadeOut(EnterMotionSpec)
                            },
                            label = "modelNameFade"
                        ) { label ->
                            Text(
                                text = label,
                                style = valueStyleFor(label),
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis
                            )
                        }
                    }
                    Icon(
                        imageVector = Icons.Outlined.ChevronRight,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.size(20.dp)
                    )
                }

                // 倍率：模型固有倍率的分段直选；未装模型时隐藏。
                if (availableScales.isNotEmpty()) {
                    val scaleLabel = stringResource(R.string.config_label_scale)
                    SingleChoiceSegmentedButtonRow(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(horizontal = Spacing.lg)
                            .padding(bottom = Spacing.md)
                            .horizontalScroll(rememberScrollState())
                            .semantics { contentDescription = scaleLabel }
                    ) {
                        availableScales.forEachIndexed { index, scale ->
                            SegmentedButton(
                                selected = selectedModel?.scaleFactor == scale,
                                onClick = { onPickScale(scale) },
                                shape = SegmentedButtonDefaults.itemShape(
                                    index = index,
                                    count = availableScales.size
                                ),
                                label = {
                                    Text(
                                        text = stringResource(R.string.config_scale_value, scale),
                                        style = ValueMono
                                    )
                                }
                            )
                        }
                    }
                }
            }

            // 输出路径：标题 + 路径副行的标准两行行样式，点击整行选目录；
            // 自定义目录时提供「使用默认」快捷操作，不再挤在行尾按钮里。
            Column(
                modifier = Modifier.fillMaxWidth()
            ) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .pressable(onClick = onSelectOutputFolder)
                        .padding(horizontal = Spacing.lg, vertical = Spacing.sm),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Icon(
                        imageVector = Icons.Outlined.FolderOpen,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                    Column(
                        modifier = Modifier
                            .weight(1f)
                            .padding(start = Spacing.md),
                        verticalArrangement = Arrangement.spacedBy(1.dp)
                    ) {
                        Text(
                            text = stringResource(R.string.home_output_path_label),
                            style = MaterialTheme.typography.bodyLarge
                        )
                        Text(
                            text = outputDirectoryLabel,
                            style = valueStyleFor(outputDirectoryLabel),
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            maxLines = 2,
                            overflow = TextOverflow.Ellipsis
                        )
                    }
                    Icon(
                        imageVector = Icons.Outlined.ChevronRight,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier
                            .padding(start = Spacing.sm)
                            .size(20.dp)
                    )
                }
                if (hasCustomOutputDirectory) {
                    TextButton(
                        onClick = onUseDefaultOutputFolder,
                        modifier = Modifier.padding(start = Spacing.lg)
                    ) {
                        Text(stringResource(R.string.home_use_default_output))
                    }
                }
            }

            ListItem(
                headlineContent = { Text(stringResource(R.string.config_advanced_title)) },
                leadingContent = {
                    Icon(
                        imageVector = Icons.Outlined.Tune,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                },
                trailingContent = {
                    Icon(
                        imageVector = Icons.Outlined.ExpandMore,
                        contentDescription = stringResource(
                            if (advancedExpanded) R.string.common_collapse else R.string.common_expand
                        ),
                        tint = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier
                            .size(20.dp)
                            .rotate(expandIconRotation)
                    )
                },
                colors = transparent,
                modifier = Modifier.pressable { advancedExpanded = !advancedExpanded }
            )

            if (advancedExpanded) {
                Column {
                    // 覆盖原图开启时输出写回原路径，格式不可生效：禁用该行。
                    ListItem(
                        headlineContent = { Text(stringResource(R.string.config_output_format)) },
                        supportingContent = {
                            // 等宽字体无 CJK 字形：中文标签走系统字体，仅拉丁格式名用 mono。
                            Text(
                                text = outputFormatLabel,
                                style = if (outputFormatLabel.any { it.code > 127 }) {
                                    MaterialTheme.typography.titleSmall
                                } else {
                                    ValueMono
                                },
                                color = MaterialTheme.colorScheme.onSurface
                            )
                        },
                        leadingContent = {
                            Icon(
                                imageVector = Icons.Outlined.Image,
                                contentDescription = null,
                                tint = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        },
                        colors = transparent,
                        modifier = Modifier.pressable(enabled = !overwriteOriginal, onClick = onPickFormat)
                    )
                    SwitchListItem(
                        icon = Icons.Outlined.Save,
                        title = stringResource(R.string.common_overwrite_original),
                        checked = overwriteOriginal,
                        onCheckedChange = onToggleOverwriteOriginal
                    )
                    SwitchListItem(
                        icon = Icons.Outlined.Backup,
                        title = stringResource(R.string.home_backup_original),
                        checked = backupOriginal,
                        enabled = overwriteOriginal,
                        onCheckedChange = onToggleBackupOriginal
                    )
                    SwitchListItem(
                        icon = Icons.Outlined.SubdirectoryArrowRight,
                        title = stringResource(R.string.home_include_subdirectories),
                        checked = processNestedFolders,
                        onCheckedChange = onToggleProcessNestedFolders
                    )
                    Spacer(modifier = Modifier.height(Spacing.sm))
                }
            }
        }
    }
}

/** 开关行：整行 toggleable（M3 官方模式），Switch 只作状态指示。 */
@Composable
private fun SwitchListItem(
    icon: ImageVector,
    title: String,
    checked: Boolean,
    enabled: Boolean = true,
    onCheckedChange: (Boolean) -> Unit
) {
    ListItem(
        headlineContent = { Text(text = title) },
        leadingContent = {
            Icon(
                imageVector = icon,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant
            )
        },
        trailingContent = {
            Switch(checked = checked, onCheckedChange = null, enabled = enabled)
        },
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        modifier = Modifier.toggleable(
            value = checked,
            enabled = enabled,
            role = Role.Switch,
            onValueChange = onCheckedChange
        )
    )
}
