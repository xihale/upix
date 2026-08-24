package com.crossscale.ui.components

import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import com.crossscale.ui.theme.SectionLabelStyle
import com.crossscale.ui.theme.Spacing

/**
 * 统一分区标题：SectionLabelStyle（labelLarge + 宽字距，主题层定义）+
 * onSurfaceVariant，置于分组卡上方。
 */
@Composable
fun SectionHeader(
    text: String,
    modifier: Modifier = Modifier,
    color: Color = MaterialTheme.colorScheme.onSurfaceVariant
) {
    Text(
        text = text,
        style = SectionLabelStyle,
        color = color,
        modifier = modifier
            .fillMaxWidth()
            .padding(horizontal = Spacing.xs, vertical = Spacing.xs)
    )
}
