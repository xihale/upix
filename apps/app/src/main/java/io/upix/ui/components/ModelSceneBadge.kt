package io.upix.ui.components

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
import androidx.compose.ui.res.stringResource
import io.upix.R
import io.upix.data.model.ModelScene
import io.upix.ui.theme.Spacing

/**
 * 模型场景徽标：适用场景标签（中性容器底）+ 可选「推荐」徽标（primaryContainer 柔和主色底，
 * 遵循 DESIGN_LANGUAGE.md §1.3 & §6，不引入 0.5dp 非标描边或名单外 primary 文本）。
 * 目录外模型传 null 不占位。
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
            containerColor = MaterialTheme.colorScheme.surfaceContainerHighest,
            contentColor = MaterialTheme.colorScheme.onSurfaceVariant
        )
        if (scene.recommended) {
            SceneTag(
                text = stringResource(R.string.model_scene_recommended),
                containerColor = MaterialTheme.colorScheme.primaryContainer,
                contentColor = MaterialTheme.colorScheme.onPrimaryContainer
            )
        }
    }
}

@Composable
private fun SceneTag(text: String, containerColor: Color, contentColor: Color) {
    Surface(
        shape = MaterialTheme.shapes.extraSmall,
        color = containerColor
    ) {
        Text(
            text = text,
            style = MaterialTheme.typography.labelSmall,
            color = contentColor,
            modifier = Modifier.padding(horizontal = Spacing.xs)
        )
    }
}
