package com.crossscale.ui.components

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp

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
    Card(modifier = modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            Text(text = title, style = MaterialTheme.typography.titleMedium)
            Text(text = summary, style = MaterialTheme.typography.bodyMedium)
            actions()

            if (showDetailsToggle) {
                TextButton(onClick = onToggleExpanded) {
                    Text(if (expanded) "收起详情" else "展开详情")
                }
            }

            AnimatedVisibility(visible = expanded && showDetailsToggle) {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    guide?.takeIf { text -> text.isNotBlank() }?.let { text ->
                        Text(text = text, style = MaterialTheme.typography.bodySmall)
                    }
                    details()
                }
            }
        }
    }
}
