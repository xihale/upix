package io.upix.ui.screens.home

import androidx.compose.animation.animateContentSize
import androidx.compose.animation.core.animateFloatAsState
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
import androidx.compose.material.icons.outlined.Layers
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
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import io.upix.R
import io.upix.data.model.ModelInfo
import io.upix.ui.theme.ExpandSizeSpec
import io.upix.ui.theme.Spacing
import io.upix.ui.theme.ValueMono
import io.upix.ui.theme.pressable
import io.upix.ui.theme.valueStyleFor

/**
 * 参数卡：一张容器内聚合所有任务配置。
 * 行组件全部使用 M3 官方 [ListItem]（正确的字阶/高度/内边距），
 * 倍率用官方 [SingleChoiceSegmentedButtonRow] 直接切换（不再经 sheet）；
 * 开关行整行 toggleable + Switch 仅作指示（M3 官方模式）。
 */
@Composable
internal fun ParameterCard(
    selectedModel: ModelInfo?,
    selectModelLabel: String,
    outputDirectoryLabel: String,
    overwriteOriginal: Boolean,
    backupOriginal: Boolean,
    processNestedFolders: Boolean,
    skipSrMarkedInputs: Boolean = true,
    outputFormatLabel: String = "",
    outputFormatFollowsOriginal: Boolean = true,
    outputFormatFollowLabel: String = "",
    availableScales: List<Int> = emptyList(),
    onPickModel: () -> Unit,
    onPickScale: (Int) -> Unit = {},
    onPickFormat: () -> Unit = {},
    onSelectOutputFolder: () -> Unit,
    onToggleOverwriteOriginal: (Boolean) -> Unit,
    onToggleBackupOriginal: (Boolean) -> Unit,
    onToggleProcessNestedFolders: (Boolean) -> Unit,
    onToggleSkipSrMarkedInputs: (Boolean) -> Unit = {}
) {
    var advancedExpanded by remember { mutableStateOf(false) }
    // 覆盖原图开启时输出写回原路径，输出目录与输出格式均不生效：整组锁定。
    val overwriteUsable = overwriteSwitchEnabled(
        outputFormatFollowsOriginal = outputFormatFollowsOriginal,
        overwriteOriginal = overwriteOriginal
    )
    val expandIconRotation by animateFloatAsState(
        targetValue = if (advancedExpanded) 180f else 0f,
        label = "advancedExpandRotation"
    )
    val transparent = ListItemDefaults.colors(containerColor = Color.Transparent)

    Surface(
        modifier = Modifier
            .fillMaxWidth()
            // 高级设置折叠区展开（180ms ease-out，统一 spec）。
            .animateContentSize(ExpandSizeSpec),
        // 设计规范：所有分组卡统一 shapes.medium（16dp），仅舞台更大。
        shape = MaterialTheme.shapes.medium,
        color = MaterialTheme.colorScheme.surfaceContainer
    ) {
        Column {
            // 模型 + 倍率是一个联动整体：倍率分段紧贴模型行下方，点倍率即换模型。
            // 单行自述：选中显模型名，未选显「选择模型」（CTA 弱色）——
            // 内容即标签，不再叠加「超分模型」标题。
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
                    val modelLabel = selectedModel?.name ?: selectModelLabel
                    Text(
                        text = modelLabel,
                        style = valueStyleFor(modelLabel),
                        color = if (selectedModel == null) {
                            MaterialTheme.colorScheme.onSurfaceVariant
                        } else {
                            Color.Unspecified
                        },
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier
                            .weight(1f)
                            .padding(start = Spacing.md)
                    )
                    Icon(
                        imageVector = Icons.Outlined.ChevronRight,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier
                            .padding(start = Spacing.sm)
                            .size(20.dp)
                    )
                }

                // 倍率：模型固有倍率的分段直选；仅一个倍率（无可切换项）时隐藏。
                if (availableScales.size > 1) {
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
                                // 不显示选中打勾：靠底色/边框区分即可，避免 xN 前多出图标。
                                icon = { },
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

            // 输出路径：单行自述，显当前生效目录——未自选时即默认 Pictures/Upix，
            // 自选后显所选路径（与默认同样不带前导 /）。覆盖原图开启时输出
            // 写回原图位置，该行不生效：不可点击且整行降透明。
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .pressable(enabled = !overwriteOriginal, onClick = onSelectOutputFolder)
                    .alpha(if (overwriteOriginal) LockedRowAlpha else 1f)
                    .padding(horizontal = Spacing.lg, vertical = Spacing.md),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Icon(
                    imageVector = Icons.Outlined.FolderOpen,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Text(
                    text = outputDirectoryLabel,
                    style = valueStyleFor(outputDirectoryLabel),
                    // 与模型名同色：默认值也是真实生效目录，不做弱化；
                    // 弱色只留给「选择模型」这类纯 CTA 占位。
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier
                        .weight(1f)
                        .padding(start = Spacing.md)
                )
                Icon(
                    imageVector = Icons.Outlined.ChevronRight,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier
                        .padding(start = Spacing.sm)
                        .size(20.dp)
                )
            }

            ListItem(
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
            ) {
                Text(stringResource(R.string.config_advanced_title))
            }

            if (advancedExpanded) {
                Column {
                    // 覆盖原图开启时输出写回原路径原格式：该行不可点击且整行降透明，
                    // 值静默切换为「跟随原图」——所见即实际行为，无需文字解释。
                    ListItem(
                        supportingContent = {
                            Column {
                                // 等宽字体无 CJK 字形：中文标签走系统字体，仅拉丁格式名用 mono。
                                val displayLabel = if (overwriteOriginal) {
                                    outputFormatFollowLabel
                                } else {
                                    outputFormatLabel
                                }
                                Text(
                                    text = displayLabel,
                                    style = if (displayLabel.any { it.code > 127 }) {
                                        MaterialTheme.typography.titleSmall
                                    } else {
                                        ValueMono
                                    },
                                    color = MaterialTheme.colorScheme.onSurface
                                )
                            }
                        },
                        leadingContent = {
                            Icon(
                                imageVector = Icons.Outlined.Image,
                                contentDescription = null,
                                tint = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        },
                        colors = transparent,
                        modifier = Modifier
                            .alpha(if (overwriteOriginal) LockedRowAlpha else 1f)
                            .pressable(enabled = !overwriteOriginal, onClick = onPickFormat)
                    ) {
                        Text(stringResource(R.string.config_output_format))
                    }
                    // 手动选了具体输出格式（非跟随原图）时与覆盖原图冲突：开关锁定为关，
                    // 整行降透明；已开启时保持可用以便关闭（兼容旧版持久化的冲突组合）。
                    SwitchListItem(
                        icon = Icons.Outlined.Save,
                        title = stringResource(R.string.common_overwrite_original),
                        checked = overwriteOriginal,
                        enabled = overwriteUsable,
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
                    SwitchListItem(
                        icon = Icons.Outlined.Layers,
                        title = stringResource(R.string.home_skip_sr_marked),
                        checked = skipSrMarkedInputs,
                        onCheckedChange = onToggleSkipSrMarkedInputs
                    )
                    Spacer(modifier = Modifier.height(Spacing.sm))
                }
            }
        }
    }
}

/** 开关行：整行 toggleable（M3 官方模式），Switch 只作状态指示；禁用时整行降透明。 */
@Composable
private fun SwitchListItem(
    icon: ImageVector,
    title: String,
    checked: Boolean,
    enabled: Boolean = true,
    onCheckedChange: (Boolean) -> Unit
) {
    ListItem(
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
        modifier = Modifier
            .alpha(if (enabled) 1f else LockedRowAlpha)
            .toggleable(
                value = checked,
                enabled = enabled,
                role = Role.Switch,
                onValueChange = onCheckedChange
            )
    ) {
        Text(text = title)
    }
}

/** M3 禁用态内容透明度：冲突锁定的行整行降透明，视觉即语义，不写解释文字。 */
private const val LockedRowAlpha = 0.38f

/**
 * 覆盖原图开关可用性：手动选了具体输出格式（非跟随原图）时冲突锁定；
 * 已开启时保持可用，让用户能关回去（也兼容旧版本持久化的冲突组合）。
 */
internal fun overwriteSwitchEnabled(
    outputFormatFollowsOriginal: Boolean,
    overwriteOriginal: Boolean
): Boolean = outputFormatFollowsOriginal || overwriteOriginal
