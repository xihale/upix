package com.crossscale.ui.screens.home

import androidx.compose.animation.animateContentSize
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Backup
import androidx.compose.material.icons.outlined.ChevronRight
import androidx.compose.material.icons.outlined.ExpandMore
import androidx.compose.material.icons.outlined.FolderOpen
import androidx.compose.material.icons.outlined.Memory
import androidx.compose.material.icons.outlined.Save
import androidx.compose.material.icons.outlined.SubdirectoryArrowRight
import androidx.compose.material.icons.outlined.Tune
import androidx.compose.material.icons.outlined.ZoomOutMap
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
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
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.crossscale.R
import com.crossscale.data.model.ModelInfo
import com.crossscale.ui.components.ConfigRow
import com.crossscale.ui.components.ConfigSwitchRow
import com.crossscale.ui.theme.ExpandSizeSpec
import com.crossscale.ui.theme.Spacing
import com.crossscale.ui.theme.ValueMono

/**
 * 参数卡：一张容器内聚合所有任务配置。
 * 常用行（模型 / 倍率 / 输出路径）+「高级设置」折叠区，
 * 行间用留白分隔。
 */
@Composable
internal fun ParameterCard(
    selectedModel: ModelInfo?,
    notSelectedLabel: String,
    outputDirectoryLabel: String,
    overwriteOriginal: Boolean,
    backupOriginal: Boolean,
    processNestedFolders: Boolean,
    onPickModel: () -> Unit,
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

    Surface(
        modifier = Modifier
            .fillMaxWidth()
            // 高级设置折叠区展开（250ms ease-out，统一 spec）。
            .animateContentSize(ExpandSizeSpec),
        shape = MaterialTheme.shapes.large,
        color = MaterialTheme.colorScheme.surfaceContainer
    ) {
        Column(
            modifier = Modifier.padding(Spacing.lg),
            verticalArrangement = Arrangement.spacedBy(Spacing.md)
        ) {
            ConfigRow(
                icon = Icons.Outlined.Memory,
                title = stringResource(R.string.home_model_setting_label),
                value = selectedModel?.name ?: notSelectedLabel,
                valueStyle = ValueMono,
                onClick = onPickModel,
                trailing = {
                    Icon(
                        imageVector = Icons.Outlined.ChevronRight,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.size(20.dp)
                    )
                }
            )

            ConfigRow(
                icon = Icons.Outlined.ZoomOutMap,
                title = stringResource(R.string.config_label_scale),
                value = selectedModel?.let { model ->
                    stringResource(R.string.config_scale_value, model.scaleFactor)
                } ?: notSelectedLabel,
                valueStyle = ValueMono,
                onClick = onPickModel
            )

            ConfigRow(
                icon = Icons.Outlined.FolderOpen,
                title = stringResource(R.string.home_output_path_label),
                value = outputDirectoryLabel,
                valueStyle = ValueMono,
                onClick = onSelectOutputFolder,
                trailing = {
                    TextButton(onClick = onUseDefaultOutputFolder) {
                        Text(stringResource(R.string.home_use_default_output))
                    }
                }
            )

            ConfigRow(
                icon = Icons.Outlined.Tune,
                title = stringResource(R.string.config_advanced_title),
                onClick = { advancedExpanded = !advancedExpanded },
                trailing = {
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
                }
            )

            if (advancedExpanded) {
                Column(verticalArrangement = Arrangement.spacedBy(Spacing.md)) {
                    selectedModel?.let { model ->
                        ConfigRow(
                            icon = Icons.Outlined.Memory,
                            title = stringResource(R.string.config_label_spec),
                            value = stringResource(
                                R.string.config_model_spec_value,
                                model.scaleFactor,
                                model.tileSize,
                                model.inputChannels
                            ),
                            valueStyle = ValueMono
                        )
                    }
                    ConfigSwitchRow(
                        icon = Icons.Outlined.Save,
                        title = stringResource(R.string.common_overwrite_original),
                        checked = overwriteOriginal,
                        onCheckedChange = onToggleOverwriteOriginal
                    )
                    ConfigSwitchRow(
                        icon = Icons.Outlined.Backup,
                        title = stringResource(R.string.home_backup_original),
                        checked = backupOriginal,
                        enabled = overwriteOriginal,
                        onCheckedChange = onToggleBackupOriginal
                    )
                    ConfigSwitchRow(
                        icon = Icons.Outlined.SubdirectoryArrowRight,
                        title = stringResource(R.string.home_include_subdirectories),
                        checked = processNestedFolders,
                        onCheckedChange = onToggleProcessNestedFolders
                    )
                }
            }
        }
    }
}
