package com.crossscale.ui.screens.home

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.AddPhotoAlternate
import androidx.compose.material.icons.outlined.Close
import androidx.compose.material.icons.outlined.CreateNewFolder
import androidx.compose.material.icons.outlined.DeleteSweep
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.crossscale.R
import com.crossscale.data.thumbnails.decodeQueueThumbnailWithLimit
import com.crossscale.ui.theme.Spacing
import com.crossscale.ui.theme.pressable

/**
 * 图片舞台：超分工具的唯一视觉焦点。
 * 空态展示品牌 motif + 两个显式入口；已选态大图铺满 + 数量角标；
 * 处理中由调用方通过 [overlay] 叠加遮罩进度。
 */
@Composable
internal fun ImageStage(
    selectedInputs: List<String>,
    corruptedSources: Set<String>,
    isProcessing: Boolean,
    onAddImage: () -> Unit,
    onAddFolder: () -> Unit,
    onClearInput: () -> Unit,
    onRemoveInput: (String) -> Unit,
    onPreviewInput: (String) -> Unit,
    overlay: (@Composable BoxScope.() -> Unit)? = null
) {
    Column(verticalArrangement = Arrangement.spacedBy(Spacing.md)) {
        Surface(
            modifier = Modifier
                .fillMaxWidth()
                .height(320.dp),
            shape = MaterialTheme.shapes.large,
            color = MaterialTheme.colorScheme.surfaceContainer
        ) {
            Box(modifier = Modifier.fillMaxSize()) {
                if (selectedInputs.isEmpty()) {
                    EmptyStage(
                        onAddImage = onAddImage,
                        onAddFolder = onAddFolder
                    )
                } else {
                    SelectedStage(
                        selectedInputs = selectedInputs,
                        isProcessing = isProcessing
                    )
                }
                overlay?.invoke(this)
            }
        }

        if (selectedInputs.isNotEmpty()) {
            ThumbnailRow(
                selectedInputs = selectedInputs,
                corruptedSources = corruptedSources,
                onAddImage = onAddImage,
                onAddFolder = onAddFolder,
                onClearInput = onClearInput,
                onRemoveInput = onRemoveInput,
                onPreviewInput = onPreviewInput
            )
        }
    }
}

/** 品牌空态：两个错位圆角方框呼应「分层放大」的启动图标。 */
@Composable
private fun EmptyStage(
    onAddImage: () -> Unit,
    onAddFolder: () -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .border(
                width = 1.5.dp,
                color = MaterialTheme.colorScheme.outlineVariant,
                shape = MaterialTheme.shapes.large
            )
            .padding(Spacing.lg),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Box(modifier = Modifier.size(112.dp)) {
            Box(
                modifier = Modifier
                    .align(Alignment.Center)
                    .size(88.dp)
                    .border(
                        width = 3.dp,
                        color = MaterialTheme.colorScheme.primary.copy(alpha = 0.6f),
                        shape = MaterialTheme.shapes.small
                    )
            )
            Box(
                modifier = Modifier
                    .align(Alignment.Center)
                    .offset(x = 12.dp, y = 12.dp)
                    .size(56.dp)
                    .background(
                        color = MaterialTheme.colorScheme.primaryContainer,
                        shape = MaterialTheme.shapes.small
                    )
            )
        }
        Spacer(modifier = Modifier.height(Spacing.xl))
        Text(
            text = stringResource(R.string.hero_stage_empty_title),
            style = MaterialTheme.typography.headlineSmall
        )
        Spacer(modifier = Modifier.height(Spacing.xs))
        Text(
            text = stringResource(R.string.hero_stage_empty_subtitle),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Spacer(modifier = Modifier.height(Spacing.xl))
        Row(horizontalArrangement = Arrangement.spacedBy(Spacing.md)) {
            FilledTonalButton(onClick = onAddImage) {
                Text(stringResource(R.string.hero_pick_image))
            }
            OutlinedButton(onClick = onAddFolder) {
                Text(stringResource(R.string.hero_pick_folder))
            }
        }
    }
}

@Composable
private fun SelectedStage(
    selectedInputs: List<String>,
    isProcessing: Boolean
) {
    val context = LocalContext.current
    val firstSource = selectedInputs.firstOrNull()
    val stageBitmap by produceState<ImageBitmap?>(initialValue = null, firstSource) {
        value = firstSource?.let { source ->
            runCatching {
                decodeQueueThumbnailWithLimit(context = context, source = source)
            }.getOrNull()
        }
    }

    Box(modifier = Modifier.fillMaxSize()) {
        val bitmap = stageBitmap
        if (bitmap != null) {
            Image(
                bitmap = bitmap,
                contentDescription = null,
                contentScale = ContentScale.Crop,
                modifier = Modifier.fillMaxSize()
            )
        }
        if (selectedInputs.size > 1 && !isProcessing) {
            Surface(
                modifier = Modifier
                    .align(Alignment.BottomEnd)
                    .padding(Spacing.md)
                    .size(28.dp),
                shape = CircleShape,
                color = MaterialTheme.colorScheme.primary,
                contentColor = MaterialTheme.colorScheme.onPrimary
            ) {
                Box(contentAlignment = Alignment.Center) {
                    Text(
                        text = selectedInputs.size.toString(),
                        style = MaterialTheme.typography.labelMedium
                    )
                }
            }
        }
    }
}

/**
 * 缩略图行：单击 = 预览，右上角 X = 删除；
 * 行尾收敛三个动作入口（加图 / 加文件夹 / 清空）。
 */
@Composable
private fun ThumbnailRow(
    selectedInputs: List<String>,
    corruptedSources: Set<String>,
    onAddImage: () -> Unit,
    onAddFolder: () -> Unit,
    onClearInput: () -> Unit,
    onRemoveInput: (String) -> Unit,
    onPreviewInput: (String) -> Unit
) {
    LazyRow(
        horizontalArrangement = Arrangement.spacedBy(Spacing.sm),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(
            horizontal = Spacing.xs,
            vertical = Spacing.xs
        )
    ) {
        items(items = selectedInputs, key = { source -> source }) { source ->
            StageThumbnail(
                source = source,
                isCorrupted = source in corruptedSources,
                onDelete = { onRemoveInput(source) },
                onPreview = { onPreviewInput(source) }
            )
        }
        item(key = "stage-add-image") {
            StageActionTile(
                icon = Icons.Outlined.AddPhotoAlternate,
                contentDescriptionRes = R.string.hero_add_image_cd,
                onClick = onAddImage
            )
        }
        item(key = "stage-add-folder") {
            StageActionTile(
                icon = Icons.Outlined.CreateNewFolder,
                contentDescriptionRes = R.string.hero_add_folder_cd,
                onClick = onAddFolder
            )
        }
        item(key = "stage-clear") {
            StageActionTile(
                icon = Icons.Outlined.DeleteSweep,
                contentDescriptionRes = R.string.hero_clear_all_cd,
                onClick = onClearInput
            )
        }
    }
}

@Composable
private fun StageThumbnail(
    source: String,
    isCorrupted: Boolean,
    onDelete: () -> Unit,
    onPreview: () -> Unit
) {
    val context = LocalContext.current
    val bitmapState by produceState<ImageBitmap?>(initialValue = null, source) {
        value = runCatching {
            decodeQueueThumbnailWithLimit(context = context, source = source)
        }.getOrNull()
    }

    Box(modifier = Modifier.size(64.dp)) {
        Box(
            modifier = Modifier
                .fillMaxSize()
                .clip(MaterialTheme.shapes.small)
                .background(MaterialTheme.colorScheme.surfaceContainerHigh)
                .pressable(onClick = onPreview),
            contentAlignment = Alignment.Center
        ) {
            val bitmap = bitmapState
            if (bitmap != null) {
                Image(
                    bitmap = bitmap,
                    contentDescription = null,
                    modifier = Modifier.fillMaxSize(),
                    contentScale = ContentScale.Crop
                )
            } else {
                Text(
                    text = if (isCorrupted) {
                        stringResource(R.string.hero_thumbnail_corrupted)
                    } else {
                        stringResource(R.string.hero_thumbnail_preview)
                    },
                    style = MaterialTheme.typography.labelSmall,
                    color = if (isCorrupted) {
                        MaterialTheme.colorScheme.error
                    } else {
                        MaterialTheme.colorScheme.onSurfaceVariant
                    }
                )
            }
        }
        Surface(
            modifier = Modifier
                .align(Alignment.TopEnd)
                .size(18.dp),
            shape = CircleShape,
            color = MaterialTheme.colorScheme.surfaceContainerHighest,
            contentColor = MaterialTheme.colorScheme.onSurface
        ) {
            IconButton(
                onClick = onDelete,
                modifier = Modifier.size(18.dp)
            ) {
                Icon(
                    imageVector = Icons.Outlined.Close,
                    contentDescription = stringResource(R.string.hero_thumbnail_remove_cd),
                    modifier = Modifier.size(14.dp)
                )
            }
        }
    }
}

@Composable
private fun StageActionTile(
    icon: androidx.compose.ui.graphics.vector.ImageVector,
    contentDescriptionRes: Int,
    onClick: () -> Unit
) {
    Box(
        modifier = Modifier
            .size(64.dp)
            .clip(MaterialTheme.shapes.small)
            .background(MaterialTheme.colorScheme.surfaceContainerHigh)
            .pressable(onClick = onClick),
        contentAlignment = Alignment.Center
    ) {
        Icon(
            imageVector = icon,
            contentDescription = stringResource(contentDescriptionRes),
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.size(20.dp)
        )
    }
}

@Composable
@OptIn(ExperimentalMaterial3Api::class)
internal fun PreviewBottomSheet(
    source: String,
    isCorrupted: Boolean,
    onDismiss: () -> Unit
) {
    val context = LocalContext.current
    val previewBitmap by produceState<ImageBitmap?>(initialValue = null, source) {
        value = runCatching {
            decodeQueueThumbnailWithLimit(context = context, source = source)
        }.getOrNull()
    }
    val previewFallbackTitle = stringResource(R.string.hero_thumbnail_preview)
    val previewTitle = remember(source, previewFallbackTitle) {
        source.substringAfterLast('/').ifBlank {
            source.takeLast(28).ifBlank { previewFallbackTitle }
        }
    }

    ModalBottomSheet(
        onDismissRequest = onDismiss,
        shape = MaterialTheme.shapes.extraLarge,
        containerColor = MaterialTheme.colorScheme.surface
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = Spacing.xl),
            verticalArrangement = Arrangement.spacedBy(Spacing.md)
        ) {
            Text(
                text = stringResource(R.string.hero_preview_title, previewTitle),
                style = MaterialTheme.typography.titleMedium
            )
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = 220.dp, max = 520.dp)
                    .clip(MaterialTheme.shapes.large)
                    .background(MaterialTheme.colorScheme.surfaceContainerHigh),
                contentAlignment = Alignment.Center
            ) {
                val bitmap = previewBitmap
                if (bitmap == null) {
                    Text(
                        text = if (isCorrupted) {
                            stringResource(R.string.hero_image_corrupted)
                        } else {
                            stringResource(R.string.hero_no_preview)
                        },
                        style = MaterialTheme.typography.bodyMedium,
                        color = if (isCorrupted) {
                            MaterialTheme.colorScheme.error
                        } else {
                            MaterialTheme.colorScheme.onSurfaceVariant
                        }
                    )
                } else {
                    Image(
                        bitmap = bitmap,
                        contentDescription = null,
                        modifier = Modifier.fillMaxSize(),
                        contentScale = ContentScale.Fit
                    )
                }
            }
            Spacer(modifier = Modifier.height(Spacing.lg))
        }
    }
}
