package io.upix.ui.components

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import io.upix.data.model.ModelScene
import io.upix.ui.theme.Spacing

/**
 * 模型场景徽标：适用场景标签（描边弱色）+ 可选「推荐」星标（主色描边，
 * 与桌面端 QML 的 amber 推荐徽标同语义）。目录外模型传 null 不占位。
 */
@Composable
fun ModelSceneBadge(scene: ModelScene?) {
    if (scene == null) return
    Row(
        horizontalArrangement = Arrangement.spacedBy(Spacing.xs),
        verticalAlignment = Alignment.CenterVertically
    ) {
        SceneTag(
            text = scene.label,
            contentColor = MaterialTheme.colorScheme.onSurfaceVariant,
            outlineColor = MaterialTheme.colorScheme.outlineVariant
        )
        if (scene.recommended) {
            SceneTag(
                text = "推荐",
                contentColor = MaterialTheme.colorScheme.primary,
                outlineColor = MaterialTheme.colorScheme.primary
            )
        }
    }
}

@Composable
private fun SceneTag(text: String, contentColor: Color, outlineColor: Color) {
    Surface(
        shape = MaterialTheme.shapes.extraSmall,
        color = Color.Transparent,
        border = BorderStroke(0.5.dp, outlineColor)
    ) {
        Text(
            text = text,
            style = MaterialTheme.typography.labelSmall,
            color = contentColor,
            modifier = Modifier.padding(horizontal = 4.dp, vertical = 1.dp)
        )
    }
}
