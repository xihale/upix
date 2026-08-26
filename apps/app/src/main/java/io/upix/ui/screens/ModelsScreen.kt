package com.upix.ui.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.CloudDownload
import androidx.compose.material.icons.outlined.Inventory2
import androidx.compose.material.icons.outlined.Refresh
import androidx.compose.material.icons.outlined.SaveAlt
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
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
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.upix.R
import com.upix.data.model.ModelInfo
import com.upix.data.model.ModelLibraryEntry
import com.upix.data.model.RemoteModel
import com.upix.ui.components.ModelCard
import com.upix.ui.components.RemoteModelsDialog
import com.upix.ui.theme.LocalScreenMargin
import com.upix.ui.theme.Spacing
import com.upix.viewmodel.ModelsUiState

/** 模型卡单列最小宽度：compact 手机必然单列，宽屏 detail pane 自动升为多列。 */
private val ModelColumnMinWidth = 320.dp

/**
 * 模型管理屏：单个滚动容器内的四段结构 ——
 * 头部元数据（数量摘要 + 模型目录）→ 操作行（导入 / 远程下载 / 刷新）
 * → 模型库自适应网格（手机单列即列表，medium+ 宽面板自动多列）
 * 或居中空态引导卡。
 * 删除确认与远程下载对话框逻辑不变；所有回调由调用方原样注入。
 */
@Composable
fun ModelsScreen(
    /** 双端合并后的模型库（含手机/PC 存在标记）。 */
    library: List<ModelLibraryEntry>,
    /** 手机本地模型（供云端下载去重与删除操作）。 */
    phoneModelNames: Set<String>,
    /** 是否已连接 PC：离线时 PC 相关徽标与传输动作无意义，不展示。 */
    pcConnected: Boolean,
    onRefreshModels: () -> Unit,
    onImportModels: () -> Unit,
    onDeleteModel: (ModelInfo) -> Unit,
    modelDirectoryPath: String,
    uploadingToPc: Set<String>,
    downloadingFromPc: Set<String>,
    onUploadToPc: (ModelLibraryEntry) -> Unit,
    onDownloadFromPc: (ModelLibraryEntry) -> Unit,
    remoteUiState: ModelsUiState,
    onRefreshRemoteModels: () -> Unit,
    onDownloadRemoteModel: (RemoteModel) -> Unit,
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

    LazyVerticalGrid(
        columns = GridCells.Adaptive(minSize = ModelColumnMinWidth),
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(
            start = LocalScreenMargin.current,
            end = LocalScreenMargin.current,
            top = Spacing.md,
            bottom = Spacing.xxl
        ),
        horizontalArrangement = Arrangement.spacedBy(Spacing.md),
        verticalArrangement = Arrangement.spacedBy(Spacing.md)
    ) {
        item(key = "header", span = { GridItemSpan(maxLineSpan) }) {
            ModelsHeader(
                libraryCount = library.size,
                showSummary = library.isNotEmpty(),
                modelDirectoryPath = modelDirectoryPath
            )
        }

        item(key = "actions", span = { GridItemSpan(maxLineSpan) }) {
            ModelsActionsRow(
                onImportModels = onImportModels,
                onOpenRemoteModels = { showRemoteDialog = true },
                onRefreshModels = onRefreshModels
            )
        }

        if (library.isEmpty()) {
            item(key = "empty", span = { GridItemSpan(maxLineSpan) }) {
                ModelsEmptyState()
            }
        } else {
            items(items = library, key = { it.name }) { entry ->
                // ModelCard 不收 modifier，包一层以获得网格位移动效。
                Box(modifier = Modifier.animateItem()) {
                    ModelCard(
                        entry = entry,
                        uploading = entry.name in uploadingToPc,
                        downloading = entry.name in downloadingFromPc,
                        pcConnected = pcConnected,
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

/** 头部元数据：数量摘要（空态时交给引导卡说，避免重复）+ 模型目录路径。 */
@Composable
private fun ModelsHeader(
    libraryCount: Int,
    showSummary: Boolean,
    modelDirectoryPath: String
) {
    Column(verticalArrangement = Arrangement.spacedBy(Spacing.xs)) {
        if (showSummary) {
            Text(
                text = stringResource(R.string.models_discovered_summary, libraryCount),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
        Text(
            text = stringResource(R.string.common_model_directory, modelDirectoryPath),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }
}

/**
 * 操作行（按钮语言收口在 §9.3 闭集）：导入 = 本屏唯一主动作（实底 Filled），
 * 远程下载 = 次级确认动作（FilledTonal，每屏至多一个），刷新 = 幽灵图标钮 ——
 * 维护动作不与任务动作争夺视觉权重，三个动作标签宽度也不再挤压。
 */
@Composable
private fun ModelsActionsRow(
    onImportModels: () -> Unit,
    onOpenRemoteModels: () -> Unit,
    onRefreshModels: () -> Unit
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(Spacing.sm)
    ) {
        Button(
            onClick = onImportModels,
            modifier = Modifier.weight(1f)
        ) {
            Icon(
                imageVector = Icons.Outlined.SaveAlt,
                contentDescription = null,
                modifier = Modifier
                    .padding(end = Spacing.xs)
                    .size(22.dp)
            )
            Text(text = stringResource(R.string.models_import), maxLines = 1)
        }
        FilledTonalButton(
            onClick = onOpenRemoteModels,
            modifier = Modifier.weight(1f)
        ) {
            Icon(
                imageVector = Icons.Outlined.CloudDownload,
                contentDescription = null,
                modifier = Modifier
                    .padding(end = Spacing.xs)
                    .size(22.dp)
            )
            Text(text = stringResource(R.string.models_download), maxLines = 1)
        }
        IconButton(onClick = onRefreshModels) {
            Icon(
                imageVector = Icons.Outlined.Refresh,
                contentDescription = stringResource(R.string.models_refresh),
                tint = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}

/**
 * 空态引导卡：居中图标徽章 + 提示标题 + 导入规则说明；
 * 导入入口复用上方操作行的「导入模型」，不再重复放按钮。
 */
@Composable
private fun ModelsEmptyState() {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        // §10.2 半径迁移：列表空态面板归 shapes.medium（16dp），与卡片族同一半径语言。
        shape = MaterialTheme.shapes.medium,
        color = MaterialTheme.colorScheme.surfaceContainer
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = Spacing.xxl, vertical = Spacing.xxl),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(Spacing.sm)
        ) {
            Box(
                modifier = Modifier
                    .size(56.dp)
                    .background(
                        color = MaterialTheme.colorScheme.surfaceContainerHighest,
                        shape = CircleShape
                    ),
                contentAlignment = Alignment.Center
            ) {
                Icon(
                    imageVector = Icons.Outlined.Inventory2,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.size(26.dp)
                )
            }
            Text(
                text = stringResource(R.string.models_empty_hint),
                style = MaterialTheme.typography.titleMedium
            )
            Text(
                text = stringResource(R.string.models_import_rules),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                textAlign = TextAlign.Center
            )
        }
    }
}
