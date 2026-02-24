package com.crossscale.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp

@Composable
fun HelpScreen() {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        Text(text = "使用帮助", style = MaterialTheme.typography.titleMedium)
        Text(text = "1. 在主页添加图片或文件夹。")
        Text(text = "2. 在设置 > 模型管理 中选择模型。")
        Text(text = "3. 默认输出到 Picture/CrossScale，可选择覆盖原图。")
        Text(text = "4. 点击开始超分后会自动加入队列并启动。")
        Text(text = "5. 队列支持简单/详细视图切换，详细视图可看缩略图和任务信息。")
    }
}
