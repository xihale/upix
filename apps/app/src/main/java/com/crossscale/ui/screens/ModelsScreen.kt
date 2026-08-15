package com.crossscale.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Download
import androidx.compose.material.icons.outlined.Refresh
import androidx.compose.material.icons.outlined.SaveAlt
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.crossscale.R
import com.crossscale.data.model.ModelInfo
import com.crossscale.ui.components.ModelCard
import com.crossscale.ui.theme.Spacing

@Composable
fun ModelsScreen(
    models: List<ModelInfo>,
    selectedModel: ModelInfo?,
    onSelectModel: (ModelInfo) -> Unit,
    onRefreshModels: () -> Unit,
    onImportModels: () -> Unit,
    onDeleteModel: (ModelInfo) -> Unit,
    modelDirectoryPath: String,
    onOpenModelRelease: () -> Unit
) {
    val notSelectedLabel = stringResource(R.string.common_not_selected)
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(horizontal = Spacing.xl)
            .padding(top = Spacing.md),
        verticalArrangement = Arrangement.spacedBy(Spacing.md)
    ) {
        Text(
            text = stringResource(R.string.models_title),
            style = MaterialTheme.typography.titleLarge
        )
        Text(
            text = stringResource(
                R.string.models_discovered_summary,
                models.size,
                selectedModel?.name ?: notSelectedLabel
            ),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )

        Row(horizontalArrangement = Arrangement.spacedBy(Spacing.sm)) {
            FilledTonalButton(onClick = onRefreshModels, modifier = Modifier.weight(1f)) {
                Icon(
                    imageVector = Icons.Outlined.Refresh,
                    contentDescription = null,
                    modifier = Modifier.padding(end = Spacing.xs).size(18.dp)
                )
                Text(stringResource(R.string.models_refresh), maxLines = 1)
            }
            OutlinedButton(onClick = onImportModels, modifier = Modifier.weight(1f)) {
                Icon(
                    imageVector = Icons.Outlined.SaveAlt,
                    contentDescription = null,
                    modifier = Modifier.padding(end = Spacing.xs).size(18.dp)
                )
                Text(stringResource(R.string.models_import), maxLines = 1)
            }
            OutlinedButton(onClick = onOpenModelRelease) {
                Icon(
                    imageVector = Icons.Outlined.Download,
                    contentDescription = stringResource(R.string.models_download),
                    modifier = Modifier.size(18.dp)
                )
            }
        }

        Text(
            text = stringResource(R.string.common_model_directory, modelDirectoryPath),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )

        if (models.isEmpty()) {
            Surface(
                modifier = Modifier.fillMaxWidth(),
                shape = MaterialTheme.shapes.large,
                color = MaterialTheme.colorScheme.surfaceContainer
            ) {
                Column(
                    modifier = Modifier.padding(Spacing.lg),
                    verticalArrangement = Arrangement.spacedBy(Spacing.sm)
                ) {
                    Text(
                        text = stringResource(R.string.models_empty_hint),
                        style = MaterialTheme.typography.bodyMedium
                    )
                    Text(
                        text = stringResource(R.string.models_import_rules),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
        } else {
            LazyColumn(
                modifier = Modifier.fillMaxSize(),
                verticalArrangement = Arrangement.spacedBy(Spacing.md),
                contentPadding = androidx.compose.foundation.layout.PaddingValues(bottom = Spacing.xxl)
            ) {
                items(items = models, key = { it.name }) { model ->
                    ModelCard(
                        model = model,
                        isSelected = selectedModel?.name == model.name,
                        onSelect = onSelectModel,
                        onDelete = onDeleteModel
                    )
                }
            }
        }
    }
}
