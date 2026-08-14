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
import androidx.compose.ui.res.stringResource
import com.crossscale.R
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
                    text = stringResource(R.string.models_current),
                    style = MaterialTheme.typography.labelLarge,
                    color = MaterialTheme.colorScheme.primary
                )
            }
        }

        Text(
            text = stringResource(
                R.string.models_card_summary,
                model.scaleFactor,
                model.totalSize / 1024 / 1024
            ),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )

        Row(horizontalArrangement = Arrangement.spacedBy(BorderlessTokens.SectionCompactSpacing)) {
            Button(onClick = { onSelect(model) }) {
                Text(
                    if (isSelected) {
                        stringResource(R.string.models_selected)
                    } else {
                        stringResource(R.string.models_select)
                    }
                )
            }
            OutlinedButton(onClick = { onDelete(model) }) {
                Text(stringResource(R.string.models_delete))
            }
        }

        HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = BorderlessTokens.DividerMediumAlpha))
    }
}
