package com.crossscale.ui.components

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.EnterTransition
import androidx.compose.animation.ExitTransition
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import com.crossscale.ui.theme.BorderlessTokens

@Composable
fun LayeredSectionCard(
    title: String,
    summary: String,
    guide: String? = null,
    expanded: Boolean,
    onToggleExpanded: () -> Unit,
    showDetailsToggle: Boolean = true,
    modifier: Modifier = Modifier,
    actions: @Composable () -> Unit,
    details: @Composable () -> Unit = {}
) {
    Column(
        modifier = modifier
            .fillMaxWidth()
            .padding(vertical = BorderlessTokens.SectionCompactSpacing),
        verticalArrangement = Arrangement.spacedBy(BorderlessTokens.SectionSpacing)
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Column(
                modifier = Modifier.weight(1f),
                verticalArrangement = Arrangement.spacedBy(BorderlessTokens.SectionCompactSpacing / 2)
            ) {
                Text(text = title, style = MaterialTheme.typography.titleMedium)
                Text(
                    text = summary,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }

            if (showDetailsToggle) {
                TextButton(onClick = onToggleExpanded) {
                    Text(if (expanded) "收起" else "详情")
                }
            }
        }

        actions()

        val detailsVisible = if (showDetailsToggle) expanded else true
        AnimatedVisibility(
            visible = detailsVisible,
            enter = EnterTransition.None,
            exit = ExitTransition.None
        ) {
            Column(verticalArrangement = Arrangement.spacedBy(BorderlessTokens.SectionCompactSpacing)) {
                guide?.takeIf { text -> text.isNotBlank() }?.let { text ->
                    Text(
                        text = text,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
                details()
            }
        }

        HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerMediumAlpha))
    }
}
