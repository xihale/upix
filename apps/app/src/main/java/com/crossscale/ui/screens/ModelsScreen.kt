package com.crossscale.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import com.crossscale.data.model.ModelInfo
import com.crossscale.ui.components.ModelCard
import com.crossscale.ui.theme.BorderlessTokens

@Composable
fun ModelsScreen(
    models: List<ModelInfo>,
    selectedModel: ModelInfo?,
    onSelectModel: (ModelInfo) -> Unit,
    onRefreshModels: () -> Unit,
    onImportModels: () -> Unit,
    onDeleteModel: (ModelInfo) -> Unit,
    modelDirectoryPath: String,
    onOpenModelRelease: () -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(
                horizontal = BorderlessTokens.ScreenPaddingHorizontal,
                vertical = BorderlessTokens.ScreenPaddingVertical
            ),
        verticalArrangement = Arrangement.spacedBy(BorderlessTokens.SectionSpacing)
    ) {
        Text(
            text = "模型",
            style = MaterialTheme.typography.titleMedium,
            fontWeight = BorderlessTokens.SectionTitleWeight
        )
        Text(
            text = "已发现 ${models.size} 个模型；当前：${selectedModel?.name ?: "未选择"}",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        HorizontalDivider(
            color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerStrongAlpha)
        )

        Button(
            onClick = onRefreshModels,
            modifier = Modifier
                .fillMaxWidth()
                .heightIn(min = BorderlessTokens.InteractiveRowMinHeight)
        ) {
            Text("刷新模型列表")
        }
        Button(
            onClick = onImportModels,
            modifier = Modifier
                .fillMaxWidth()
                .heightIn(min = BorderlessTokens.InteractiveRowMinHeight)
        ) {
            Text("导入模型（.param/.bin/.zip）")
        }
        OutlinedButton(
            onClick = onOpenModelRelease,
            modifier = Modifier
                .fillMaxWidth()
                .heightIn(min = BorderlessTokens.InteractiveRowMinHeight)
        ) {
            Text("下载模型（占位）")
        }
        Text(
            text = "模型目录：$modelDirectoryPath",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Text(
            text = "导入规则：必须成对同名 .param + .bin；zip 支持多模型与多层文件夹。",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )

        HorizontalDivider(
            color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerStrongAlpha)
        )

        if (models.isEmpty()) {
            Text(
                text = "未发现模型，请将 .param/.bin 放入模型目录",
                style = MaterialTheme.typography.bodyMedium
            )
        } else {
            LazyColumn(
                modifier = Modifier.fillMaxSize(),
                verticalArrangement = Arrangement.spacedBy(BorderlessTokens.ListItemSpacing)
            ) {
                items(items = models, key = { it.name }) { model ->
                    ModelCard(
                        model = model,
                        isSelected = selectedModel?.name == model.name,
                        onSelect = onSelectModel,
                        onDelete = onDeleteModel
                    )
                }
            }
        }
    }
}
