package com.crossscale.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import com.crossscale.ui.theme.BorderlessTokens

@Composable
fun HelpScreen() {
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
            text = "使用帮助",
            style = MaterialTheme.typography.headlineSmall,
            fontWeight = BorderlessTokens.SectionTitleWeight
        )
        HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerStrongAlpha))
        HelpLine("1. 在主页顶部点击“添加图片或文件夹”。")
        HelpLine("2. 选择超分模型与输出路径。")
        HelpLine("3. 根据需要开启“覆盖原图 / 备份原图”。")
        HelpLine("4. 点击底部“开始超分”，并可上拉队列查看进度与失败原因。")
        HelpLine("5. 文件与模型导入统一使用系统文件选择器。")

        Text(
            text = "提示：若开启覆盖原图，建议同时开启备份原图。",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.fillMaxWidth()
        )
    }
}

@Composable
private fun HelpLine(text: String) {
    Text(
        text = text,
        style = MaterialTheme.typography.bodyLarge,
        modifier = Modifier.fillMaxWidth()
    )
}
