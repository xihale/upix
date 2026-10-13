package io.upix.ui.screens.home

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.CloudDownload
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.SheetValue
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalWindowInfo
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import io.upix.R
import io.upix.data.model.ModelInfo
import io.upix.data.model.classifyModelScene
import io.upix.ui.components.ModelFamilyCard
import io.upix.ui.components.ModelFamilyEmptyCard
import io.upix.ui.components.ModelListRow
import io.upix.ui.components.ModelSceneBadge
import io.upix.ui.components.SectionHeader
import io.upix.ui.components.formatSizeBytes
import io.upix.ui.components.groupModelsByFamily
import io.upix.ui.theme.Spacing
import io.upix.ui.theme.valueStyleFor

/**
 * 模型选择 sheet：目录涨到 18 个后平铺单选行放不下也难读，改为按家族分组
 * （SectionHeader + 分组卡，分组与行基座见 components/ModelFamily.kt）。
 * 选中行 = surfaceContainerHighest 圆角染色 + 前置勾，纯中性色（primary
 * 使用白名单不含列表选中指示）。列表区约七成窗口高封顶后纵向滚动。
 */
@Composable
@OptIn(ExperimentalMaterial3Api::class)
internal fun ModelPickerSheet(
    models: List<ModelInfo>,
    selectedModelName: String?,
    onDismiss: () -> Unit,
    onSelectModel: (ModelInfo) -> Unit,
    onOpenDownload: () -> Unit,
    onOpenHelp: () -> Unit = {}
) {
    val groups = remember(models) { groupModelsByFamily(models) }
    ModalBottomSheet(
        onDismissRequest = onDismiss,
        sheetState = rememberBottomSheetState(
            initialValue = SheetValue.Hidden,
            enabledValues = setOf(SheetValue.Hidden, SheetValue.Expanded)
        ),
        shape = MaterialTheme.shapes.extraLarge,
        containerColor = MaterialTheme.colorScheme.surface
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(start = Spacing.xl, end = Spacing.sm),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = stringResource(R.string.home_select_model_sheet_title),
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(vertical = Spacing.sm)
            )
            // 右上角下载入口：与设置页相同的远程模型下载 sheet。
            IconButton(onClick = onOpenDownload) {
                Icon(
                    imageVector = Icons.Outlined.CloudDownload,
                    contentDescription = stringResource(R.string.models_download),
                    tint = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
        // 分组列表限高约七成窗口：内容再多 sheet 也不吞满全屏，组内滚动可达。
        val listMaxHeight = with(LocalDensity.current) {
            (LocalWindowInfo.current.containerSize.height * 0.7f).toDp()
        }
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .heightIn(max = listMaxHeight)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = Spacing.xl)
                .padding(bottom = Spacing.xl),
            verticalArrangement = Arrangement.spacedBy(Spacing.md)
        ) {
            if (groups.isEmpty()) {
                ModelFamilyEmptyCard(text = stringResource(R.string.models_empty_hint))
            }
            groups.forEach { group ->
                Column(verticalArrangement = Arrangement.spacedBy(Spacing.sm)) {
                    if (group.label != null) {
                        SectionHeader(text = group.label)
                    }
                    ModelFamilyCard {
                        group.items.forEach { entry ->
                            val sizeText = formatSizeBytes(entry.value.totalSize)
                            ModelListRow(
                                label = entry.label,
                                isSelected = entry.value.name == selectedModelName,
                                onClick = {
                                    onSelectModel(entry.value)
                                    onDismiss()
                                },
                                badge = { ModelSceneBadge(classifyModelScene(entry.value.name)) },
                                trailing = {
                                    Text(
                                        text = sizeText,
                                        style = valueStyleFor(sizeText),
                                        color = MaterialTheme.colorScheme.onSurfaceVariant
                                    )
                                }
                            )
                        }
                    }
                }
            }
        }
        // 底部固定的指南入口：列表再长也始终可见，目录涨到 27 个后的选型兜底。
        TextButton(
            onClick = onOpenHelp,
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = Spacing.xl)
                .padding(bottom = Spacing.md)
        ) {
            Text(text = stringResource(R.string.home_model_picker_help))
        }
    }
}
