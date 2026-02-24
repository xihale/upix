package com.crossscale.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.crossscale.ui.components.LayeredSectionCard

@Composable
fun SettingsScreen(
    modelDirectoryPath: String,
    selectedModelName: String?,
    onOpenModels: () -> Unit,
    onOpenLogs: () -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        LayeredSectionCard(
            title = "设置",
            summary = "当前模型：${selectedModelName ?: "未选择"}",
            guide = null,
            expanded = false,
            onToggleExpanded = {},
            showDetailsToggle = false,
            actions = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = onOpenModels, modifier = Modifier.fillMaxWidth()) {
                        Text("模型管理")
                    }
                    OutlinedButton(onClick = onOpenLogs, modifier = Modifier.fillMaxWidth()) {
                        Text("日志")
                    }
                }
            },
            details = {}
        )
        Text(text = "模型目录：$modelDirectoryPath")
    }
}
