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
import androidx.compose.material.icons.outlined.CloudDownload
import androidx.compose.material.icons.outlined.Refresh
import androidx.compose.material.icons.outlined.SaveAlt
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.crossscale.R
import com.crossscale.data.model.ModelLibraryEntry
import com.crossscale.ui.components.ModelCard
import com.crossscale.ui.components.RemoteModelsDialog
import com.crossscale.ui.theme.LocalScreenMargin
import com.crossscale.ui.theme.Spacing
import com.crossscale.viewmodel.ModelsUiState

@Composable
fun ModelsScreen(
    /** 双端合并后的模型库（含手机/PC 存在标记）。 */
    library: List<ModelLibraryEntry>,
    /** 手机本地模型（供云端下载去重与删除操作）。 */
    phoneModelNames: Set<String>,
    onRefreshModels: () -> Unit,
    onImportModels: () -> Unit,
    onDeleteModel: (com.crossscale.data.model.ModelInfo) -> Unit,
    modelDirectoryPath: String,
    uploadingToPc: Set<String>,
    downloadingFromPc: Set<String>,
    onUploadToPc: (ModelLibraryEntry) -> Unit,
    onDownloadFromPc: (ModelLibraryEntry) -> Unit,
    remoteUiState: ModelsUiState,
    onRefreshRemoteModels: () -> Unit,
    onDownloadRemoteModel: (com.crossscale.data.model.RemoteModel) -> Unit,
    onDismissDownloadError: (String) -> Unit,
    onCancelDownload: (String) -> Unit
) {
    var showRemoteDialog by rememberSaveable { mutableStateOf(false) }
    // 删除为不可恢复操作：卡片只登记目标，实际删除前统一弹确认框。
    var deleteTarget by remember { mutableStateOf<ModelLibraryEntry?>(null) }

    // 仅在远程列表为空时自动拉取一次；已有列表时不主动联网，靠手动刷新。
    LaunchedEffect(showRemoteDialog) {
        if (showRemoteDialog && remoteUiState.remoteModels.isEmpty()) onRefreshRemoteModels()
    }
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(horizontal = LocalScreenMargin.current)
            .padding(top = Spacing.md),
        verticalArrangement = Arrangement.spacedBy(Spacing.md)
    ) {
        Text(
            text = stringResource(R.string.models_discovered_summary, library.size),
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
            OutlinedButton(onClick = { showRemoteDialog = true }) {
                Icon(
                    imageVector = Icons.Outlined.CloudDownload,
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

        if (library.isEmpty()) {
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
                items(items = library, key = { it.name }) { entry ->
                    ModelCard(
                        entry = entry,
                        uploading = entry.name in uploadingToPc,
                        downloading = entry.name in downloadingFromPc,
                        onUploadToPc = { onUploadToPc(entry) },
                        onDownloadFromPc = { onDownloadFromPc(entry) },
                        onDelete = if (entry.localModel != null) {
                            { deleteTarget = entry }
                        } else {
                            null
                        }
                    )
                }
            }
        }

        if (showRemoteDialog) {
            RemoteModelsDialog(
                remoteModels = remoteUiState.remoteModels,
                installedModelNames = phoneModelNames,
                downloads = remoteUiState.downloads,
                isLoading = remoteUiState.isLoadingRemote,
                errorMessage = remoteUiState.remoteError,
                onRefresh = onRefreshRemoteModels,
                onDownload = onDownloadRemoteModel,
                onDismiss = { showRemoteDialog = false },
                onDismissError = onDismissDownloadError,
                onCancelDownload = onCancelDownload
            )
        }
    }

    deleteTarget?.let { target ->
        AlertDialog(
            onDismissRequest = { deleteTarget = null },
            title = { Text(stringResource(R.string.models_delete_confirm_title)) },
            text = { Text(stringResource(R.string.models_delete_confirm_body, target.name)) },
            confirmButton = {
                TextButton(
                    onClick = {
                        target.localModel?.let(onDeleteModel)
                        deleteTarget = null
                    }
                ) {
                    Text(stringResource(R.string.common_confirm))
                }
            },
            dismissButton = {
                TextButton(onClick = { deleteTarget = null }) {
                    Text(stringResource(R.string.common_cancel))
                }
            }
        )
    }
}
