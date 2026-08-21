package com.crossscale.ui.screens.home

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
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
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.offset
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.Outline
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import com.crossscale.R
import com.crossscale.data.thumbnails.decodeQueueThumbnailWithLimit
import com.crossscale.ui.theme.Spacing
import com.crossscale.ui.theme.pressable

// 320dp 舞台 / 全屏预览 sheet 的解码目标：默认 192px 缩略图在放大 3-5 倍后
// 明显发糊；按展示区域给更高的解码尺寸（缓存 key 已含尺寸，互不污染）。
private const val STAGE_THUMBNAIL_TARGET_SIZE_PX = 640
private const val PREVIEW_THUMBNAIL_TARGET_SIZE_PX = 960

/**
 * 图片舞台：超分工具的唯一视觉焦点。
 * 空态只留两个显式入口；已选态大图铺满；
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
                .height(if (selectedInputs.isEmpty()) 180.dp else 320.dp),
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
                        selectedInputs = selectedInputs
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

/** 1/e：斜线上下端点在顶/底边框上的位置比例。 */
private const val INV_E = 0.36787944f

/** 斜线分割形状：斜线左侧（图片区）。 */
private val ImageZoneShape = SlashSideShape(isLeft = true)

/** 斜线分割形状：斜线右侧（文件夹区）。 */
private val FolderZoneShape = SlashSideShape(isLeft = false)

/**
 * 斜线单侧区域：下端点在底边自左 1/e 处，上端点在顶边自右 1/e 处（关于中心对称）；
 * isLeft 取线左侧四边形，否则取右侧。
 */
private class SlashSideShape(private val isLeft: Boolean) : Shape {
    override fun createOutline(
        size: Size,
        layoutDirection: LayoutDirection,
        density: Density
    ): Outline {
        val topX = size.width * (1f - INV_E)
        val bottomX = size.width * INV_E
        val path = Path()
        if (isLeft) {
            path.moveTo(0f, 0f)
            path.lineTo(topX, 0f)
            path.lineTo(bottomX, size.height)
            path.lineTo(0f, size.height)
        } else {
            path.moveTo(topX, 0f)
            path.lineTo(size.width, 0f)
            path.lineTo(size.width, size.height)
            path.lineTo(bottomX, size.height)
        }
        path.close()
        return Outline.Generic(path)
    }
}

/**
 * 空态：一条 "/" 细线安静地分割舞台（端点位于上下边框 1/e 处），
 * 左侧=图片、右侧=文件夹，点按对应区域触发相应选择。
 */
@Composable
private fun EmptyStage(
    onAddImage: () -> Unit,
    onAddFolder: () -> Unit
) {
    BoxWithConstraints(modifier = Modifier.fillMaxSize()) {
        // 两侧梯形重心在 (W/4, H/2) 与 (3W/4, H/2)：相对中心各偏移 W/4。
        val dx = maxWidth / 4

        Box(
            modifier = Modifier
                .fillMaxSize()
                .clip(ImageZoneShape)
                .clickable(onClick = onAddImage),
            contentAlignment = Alignment.Center
        ) {
            Icon(
                imageVector = Icons.Outlined.AddPhotoAlternate,
                contentDescription = stringResource(R.string.hero_add_image_cd),
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier
                    .offset(x = -dx)
                    .size(40.dp)
            )
        }

        Box(
            modifier = Modifier
                .fillMaxSize()
                .clip(FolderZoneShape)
                .clickable(onClick = onAddFolder),
            contentAlignment = Alignment.Center
        ) {
            Icon(
                imageVector = Icons.Outlined.CreateNewFolder,
                contentDescription = stringResource(R.string.hero_add_folder_cd),
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier
                    .offset(x = dx)
                    .size(40.dp)
            )
        }

        val lineColor = MaterialTheme.colorScheme.outlineVariant
        Canvas(modifier = Modifier.fillMaxSize()) {
            drawLine(
                color = lineColor,
                start = Offset(size.width * INV_E, size.height),
                end = Offset(size.width * (1f - INV_E), 0f),
                strokeWidth = 1.dp.toPx()
            )
        }
    }
}

@Composable
private fun SelectedStage(
    selectedInputs: List<String>
) {
    val context = LocalContext.current
    val firstSource = selectedInputs.firstOrNull()
    val stageBitmap by produceState<ImageBitmap?>(initialValue = null, firstSource) {
        value = firstSource?.let { source ->
            runCatching {
                decodeQueueThumbnailWithLimit(
                    context = context,
                    source = source,
                    targetSizePx = STAGE_THUMBNAIL_TARGET_SIZE_PX
                )
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

    Box(modifier = Modifier.size(72.dp)) {
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
                .offset(x = 3.dp, y = (-3).dp)
                .size(22.dp),
            shape = CircleShape,
            color = MaterialTheme.colorScheme.surfaceContainerHighest,
            contentColor = MaterialTheme.colorScheme.onSurface,
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline)
        ) {
            IconButton(
                onClick = onDelete,
                modifier = Modifier.size(22.dp)
            ) {
                Icon(
                    imageVector = Icons.Outlined.Close,
                    contentDescription = stringResource(R.string.hero_thumbnail_remove_cd),
                    modifier = Modifier.size(13.dp)
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
            .size(72.dp)
            .clip(MaterialTheme.shapes.small)
            .background(MaterialTheme.colorScheme.surfaceContainerLowest)
            .border(1.dp, MaterialTheme.colorScheme.outlineVariant, MaterialTheme.shapes.small)
            .pressable(onClick = onClick),
        contentAlignment = Alignment.Center
    ) {
        Icon(
            imageVector = icon,
            contentDescription = stringResource(contentDescriptionRes),
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.size(22.dp)
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
            decodeQueueThumbnailWithLimit(
                context = context,
                source = source,
                targetSizePx = PREVIEW_THUMBNAIL_TARGET_SIZE_PX
            )
        }.getOrNull()
    }

    ModalBottomSheet(
        onDismissRequest = onDismiss,
        sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true),
        shape = MaterialTheme.shapes.extraLarge,
        containerColor = MaterialTheme.colorScheme.surface
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = Spacing.xl),
            verticalArrangement = Arrangement.spacedBy(Spacing.md)
        ) {
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
