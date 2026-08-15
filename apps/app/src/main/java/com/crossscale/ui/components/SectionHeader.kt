package com.crossscale.ui.components

import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp
import com.crossscale.ui.theme.Spacing

/**
 * 统一分区标题：labelLarge + onSurfaceVariant + 宽字距，置于分组卡上方。
 */
@Composable
fun SectionHeader(
    text: String,
    modifier: Modifier = Modifier,
    color: Color = MaterialTheme.colorScheme.onSurfaceVariant
) {
    Text(
        text = text,
        style = MaterialTheme.typography.labelLarge.copy(
            fontWeight = FontWeight.Medium,
            letterSpacing = 1.2.sp
        ),
        color = color,
        modifier = modifier
            .fillMaxWidth()
            .padding(horizontal = Spacing.xs, vertical = Spacing.xs)
    )
}
