package com.crossscale.ui.components

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import com.crossscale.data.model.ModelInfo
import com.crossscale.ui.theme.BorderlessTokens

@Composable
fun ModelCard(
    model: ModelInfo,
    isSelected: Boolean,
    onSelect: (ModelInfo) -> Unit,
    onDelete: (ModelInfo) -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = BorderlessTokens.ListItemVerticalPadding),
        verticalArrangement = Arrangement.spacedBy(BorderlessTokens.SectionCompactSpacing)
    ) {
        Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Text(
                text = model.name,
                style = MaterialTheme.typography.titleMedium,
                fontWeight = BorderlessTokens.SectionTitleWeight
            )
            if (isSelected) {
                Text(
                    text = "当前",
                    style = MaterialTheme.typography.labelLarge,
                    color = MaterialTheme.colorScheme.primary
                )
            }
        }

        Text(
            text = "放大倍率: ${model.scaleFactor}x · 大小: ${model.totalSize / 1024 / 1024}MB",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )

        Row(horizontalArrangement = Arrangement.spacedBy(BorderlessTokens.SectionCompactSpacing)) {
            Button(onClick = { onSelect(model) }) {
                Text(if (isSelected) "已选择" else "选择")
            }
            OutlinedButton(onClick = { onDelete(model) }) {
                Text("删除")
            }
        }

        HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerMediumAlpha))
    }
}
