package com.crossscale.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.crossscale.data.model.ModelInfo
import com.crossscale.ui.components.LayeredSectionCard
import com.crossscale.ui.components.ModelCard

@Composable
fun ModelsScreen(
    models: List<ModelInfo>,
    selectedModel: ModelInfo?,
    onSelectModel: (ModelInfo) -> Unit,
    onRefreshModels: () -> Unit,
    onDeleteModel: (ModelInfo) -> Unit,
    modelDirectoryPath: String,
    onOpenModelRelease: () -> Unit
) {
    var detailsExpanded by remember { mutableStateOf(false) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        LayeredSectionCard(
            title = "模型",
            summary = "已发现 ${models.size} 个模型；当前：${selectedModel?.name ?: "未选择"}",
            guide = null,
            expanded = detailsExpanded,
            onToggleExpanded = { detailsExpanded = !detailsExpanded },
            actions = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(
                        onClick = onRefreshModels,
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Text("刷新模型列表")
                    }
                    OutlinedButton(
                        onClick = onOpenModelRelease,
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Text("下载模型（占位）")
                    }
                }
            },
            details = {
                Text(text = "模型目录：$modelDirectoryPath", style = MaterialTheme.typography.bodySmall)
                Text(text = "下载功能后续接入，目前默认跳转项目 GitHub Release。", style = MaterialTheme.typography.bodySmall)
            }
        )

        if (models.isEmpty()) {
            Text(
                text = "未发现模型，请将 .param/.bin 放入模型目录",
                style = MaterialTheme.typography.bodyMedium
            )
        } else {
            LazyColumn(
                modifier = Modifier.fillMaxSize(),
                verticalArrangement = Arrangement.spacedBy(8.dp)
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
