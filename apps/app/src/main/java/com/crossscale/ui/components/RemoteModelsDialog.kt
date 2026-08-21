package com.crossscale.ui.components

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.CloudDownload
import androidx.compose.material.icons.outlined.Refresh
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.crossscale.R
import com.crossscale.data.model.RemoteModel
import com.crossscale.ui.theme.Spacing
import com.crossscale.viewmodel.RemoteModelDownloadState
import java.util.Locale

/**
 * 远程模型下载对话框：列出 GitHub "models" release 中的模型，
 * 支持逐个下载（进度条）与失败重试。
 */
@Composable
fun RemoteModelsDialog(
    remoteModels: List<RemoteModel>,
    installedModelNames: Set<String>,
    downloads: Map<String, RemoteModelDownloadState>,
    isLoading: Boolean,
    errorMessage: String?,
    onRefresh: () -> Unit,
    onDownload: (RemoteModel) -> Unit,
    onDismiss: () -> Unit,
    onDismissError: (String) -> Unit = {}
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(stringResource(R.string.models_remote_title))
                if (isLoading) {
                    CircularProgressIndicator(
                        modifier = Modifier.size(20.dp),
                        strokeWidth = 2.dp
                    )
                } else {
                    IconButton(onClick = onRefresh) {
                        Icon(
                            imageVector = Icons.Outlined.Refresh,
                            contentDescription = stringResource(R.string.models_refresh)
                        )
                    }
                }
            }
        },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(Spacing.sm)) {
                if (errorMessage != null) {
                    Text(
                        text = errorMessage,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.error
                    )
                }
                when {
                    remoteModels.isEmpty() && isLoading -> Text(
                        text = stringResource(R.string.models_remote_loading),
                        style = MaterialTheme.typography.bodyMedium
                    )

                    remoteModels.isEmpty() -> Text(
                        text = stringResource(R.string.models_remote_empty),
                        style = MaterialTheme.typography.bodyMedium
                    )

                    else -> LazyColumn(
                        modifier = Modifier.fillMaxWidth(),
                        verticalArrangement = Arrangement.spacedBy(Spacing.sm)
                    ) {
                        items(items = remoteModels, key = { it.name }) { model ->
                            RemoteModelRow(
                                model = model,
                                installed = model.name in installedModelNames,
                                downloadState = downloads[model.name],
                                onDownload = onDownload,
                                onDismissError = onDismissError
                            )
                        }
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) {
                Text(stringResource(R.string.common_cancel))
            }
        }
    )
}

@Composable
private fun RemoteModelRow(
    model: RemoteModel,
    installed: Boolean,
    downloadState: RemoteModelDownloadState?,
    onDownload: (RemoteModel) -> Unit,
    onDismissError: (String) -> Unit
) {
    Column(verticalArrangement = Arrangement.spacedBy(Spacing.xs)) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column(Modifier.weight(1f)) {
                Text(
                    text = model.name,
                    style = MaterialTheme.typography.bodyMedium,
                    maxLines = 1
                )
                Text(
                    text = stringResource(
                        R.string.models_remote_item_summary,
                        model.scale,
                        formatSize(model.sizeBytes)
                    ),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            when {
                installed -> Text(
                    text = stringResource(R.string.models_remote_installed),
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.primary
                )

                downloadState != null && !downloadState.failed -> LinearProgressIndicator(
                    progress = { downloadState.progress },
                    modifier = Modifier.fillMaxWidth(0.35f)
                )

                downloadState != null && downloadState.failed -> TextButton(
                    onClick = {
                        onDismissError(model.name)
                        onDownload(model)
                    }
                ) {
                    Text(stringResource(R.string.models_remote_retry))
                }

                else -> IconButton(onClick = { onDownload(model) }) {
                    Icon(
                        imageVector = Icons.Outlined.CloudDownload,
                        contentDescription = stringResource(R.string.models_download)
                    )
                }
            }
        }
        if (downloadState != null && downloadState.failed && downloadState.errorMessage != null) {
            Text(
                text = downloadState.errorMessage,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error
            )
        }
    }
}

private fun formatSize(bytes: Long): String = when {
    bytes >= 1024 * 1024 -> String.format(Locale.ROOT, "%.1fMB", bytes / 1024f / 1024f)
    bytes >= 1024 -> String.format(Locale.ROOT, "%.0fKB", bytes / 1024f)
    else -> "${bytes}B"
}
