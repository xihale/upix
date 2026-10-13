package io.upix.ui.components

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.CloudDownload
import androidx.compose.material.icons.outlined.Refresh
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.SheetValue
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalWindowInfo
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import io.upix.R
import io.upix.data.model.RemoteModel
import io.upix.data.model.classifyModelScene
import io.upix.ui.theme.Spacing
import io.upix.ui.theme.valueStyleFor
import io.upix.viewmodel.RemoteModelDownloadState

/**
 * 远程模型下载 sheet：列出 GitHub "models" release 中的模型，按家族分组
 * （与模型选择 sheet 同一语言），支持逐个下载（进度条）、取消与失败重试。
 * 行状态全部收进行内：未安装=容量+下载钮；下载中=进度条+取消；失败=错误
 * 行+重试；已安装=弱色「已安装」徽标。§9.10：picker/queue 类走
 * ModalBottomSheet，不再用 AlertDialog。
 */
@Composable
@OptIn(ExperimentalMaterial3Api::class)
fun RemoteModelsSheet(
    remoteModels: List<RemoteModel>,
    installedModelNames: Set<String>,
    downloads: Map<String, RemoteModelDownloadState>,
    isLoading: Boolean,
    errorMessage: String?,
    onRefresh: () -> Unit,
    onDownload: (RemoteModel) -> Unit,
    onDismiss: () -> Unit,
    onDismissError: (String) -> Unit = {},
    onCancelDownload: (String) -> Unit = {}
) {
    val groups = remember(remoteModels) { groupByFamilyPrefix(remoteModels) { it.name } }
    ModalBottomSheet(
        onDismissRequest = onDismiss,
        sheetState = rememberBottomSheetState(
            initialValue = SheetValue.Hidden,
            enabledValues = setOf(SheetValue.Hidden, SheetValue.Expanded)
        ),
        shape = MaterialTheme.shapes.extraLarge,
        containerColor = MaterialTheme.colorScheme.surface
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(start = Spacing.xl, end = Spacing.sm),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = stringResource(R.string.models_remote_title),
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(vertical = Spacing.sm)
            )
            // 加载中不用 spinner 整个替换刷新钮（避免同一位置控件跳变）：
            // 刷新钮常驻、加载时禁用，忙碌指示器并列展示在左侧；
            // 20dp 占位常驻，避免忙碌指示器出现/消失时标题行高度跳变。
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(modifier = Modifier.size(20.dp)) {
                    if (isLoading) {
                        BusyIndicator()
                    }
                }
                IconButton(onClick = onRefresh, enabled = !isLoading) {
                    Icon(
                        imageVector = Icons.Outlined.Refresh,
                        contentDescription = stringResource(R.string.models_refresh)
                    )
                }
            }
        }
        if (errorMessage != null) {
            Text(
                text = errorMessage,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error,
                modifier = Modifier
                    .padding(horizontal = Spacing.xl)
                    .padding(bottom = Spacing.sm)
            )
        }
        // 分组列表限高约七成窗口，与模型选择 sheet 一致。
        val listMaxHeight = with(LocalDensity.current) {
            (LocalWindowInfo.current.containerSize.height * 0.7f).toDp()
        }
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .heightIn(max = listMaxHeight)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = Spacing.xl)
                .padding(bottom = Spacing.xl),
            verticalArrangement = Arrangement.spacedBy(Spacing.md)
        ) {
            when {
                remoteModels.isEmpty() && isLoading -> Text(
                    text = stringResource(R.string.models_remote_loading),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(vertical = Spacing.lg)
                )

                remoteModels.isEmpty() -> ModelFamilyEmptyCard(
                    text = stringResource(R.string.models_remote_empty)
                )

                else -> groups.forEach { group ->
                    Column(verticalArrangement = Arrangement.spacedBy(Spacing.sm)) {
                        if (group.label != null) {
                            SectionHeader(text = group.label)
                        }
                        ModelFamilyCard {
                            group.items.forEach { entry ->
                                RemoteModelRow(
                                    model = entry.value,
                                    label = entry.label,
                                    installed = entry.value.name in installedModelNames,
                                    downloadState = downloads[entry.value.name],
                                    onDownload = onDownload,
                                    onDismissError = onDismissError,
                                    onCancelDownload = onCancelDownload
                                )
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun RemoteModelRow(
    model: RemoteModel,
    label: String,
    installed: Boolean,
    downloadState: RemoteModelDownloadState?,
    onDownload: (RemoteModel) -> Unit,
    onDismissError: (String) -> Unit,
    onCancelDownload: (String) -> Unit
) {
    // 就地接住可空态：supporting/trailing 闭包里直接用这个局部 val 保住 smart cast。
    val state = downloadState
    ModelListRow(
        label = label,
        badge = { ModelSceneBadge(classifyModelScene(model.name)) },
        supporting = when {
            state != null && !state.failed && state.indeterminate -> {
                // 总大小未知（如服务端不返回 Content-Length）：不确定进度 + 已下载字节提示。
                {
                    Column(verticalArrangement = Arrangement.spacedBy(Spacing.xs)) {
                        LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
                        Text(
                            text = stringResource(
                                R.string.download_downloaded_bytes,
                                formatSizeBytes(state.downloadedBytes)
                            ),
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
            }

            state != null && !state.failed -> {
                {
                    LinearProgressIndicator(
                        progress = { state.progress },
                        modifier = Modifier.fillMaxWidth()
                    )
                }
            }

            state != null && state.failed && state.errorMessage != null -> {
                {
                    Text(
                        text = state.errorMessage,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.error
                    )
                }
            }

            else -> null
        },
        trailing = {
            when {
                installed -> Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(Spacing.sm)
                ) {
                    Text(
                        text = formatSizeBytes(model.sizeBytes),
                        style = valueStyleFor(formatSizeBytes(model.sizeBytes)),
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                    Text(
                        text = stringResource(R.string.models_remote_installed),
                        style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }

                state != null && !state.failed -> TextButton(onClick = { onCancelDownload(model.name) }) {
                    Text(
                        text = stringResource(R.string.models_download_cancel),
                        style = MaterialTheme.typography.labelMedium
                    )
                }

                state != null && state.failed -> TextButton(
                    onClick = {
                        onDismissError(model.name)
                        onDownload(model)
                    }
                ) {
                    Text(
                        text = stringResource(R.string.models_remote_retry),
                        style = MaterialTheme.typography.labelMedium
                    )
                }

                else -> Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(Spacing.sm)
                ) {
                    Text(
                        text = formatSizeBytes(model.sizeBytes),
                        style = valueStyleFor(formatSizeBytes(model.sizeBytes)),
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                    IconButton(onClick = { onDownload(model) }) {
                        Icon(
                            imageVector = Icons.Outlined.CloudDownload,
                            contentDescription = stringResource(R.string.models_download)
                        )
                    }
                }
            }
        }
    )
}
