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
    onDeleteModel: (ModelInfo) -> Unit
) {
    var helpExpanded by remember { mutableStateOf(false) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Text(
            text = "模型管理",
            style = MaterialTheme.typography.headlineSmall
        )
        Text(
            text = "先选一个默认模型即可开始；更详细的模型说明可展开查看。",
            style = MaterialTheme.typography.bodyMedium
        )

        LayeredSectionCard(
            title = "模型操作",
            summary = "已发现 ${models.size} 个模型；当前：${selectedModel?.name ?: "未选择"}",
            guide = "如果列表为空，请确认 .param/.bin 已放入 app 内部 models 目录。",
            expanded = helpExpanded,
            onToggleExpanded = { helpExpanded = !helpExpanded },
            actions = {
                Button(
                    onClick = onRefreshModels,
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Text("刷新模型列表")
                }
            },
            details = {
                Text(
                    text = "选择后会作为主页默认模型。删除模型前请确认当前任务未在运行。",
                    style = MaterialTheme.typography.bodySmall
                )
            }
        )

        if (models.isEmpty()) {
            Text(
                text = "未发现模型，请将 .param/.bin 放入 app 内部 models 目录",
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
