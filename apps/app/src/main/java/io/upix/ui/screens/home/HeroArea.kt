package com.upix.ui.screens.home

import android.net.Uri
import androidx.compose.animation.animateContentSize
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
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.detectTapGestures
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
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.Outline
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.layout
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import com.upix.R
import com.upix.data.thumbnails.decodeQueueThumbnailWithLimit
import com.upix.ui.theme.ExpandSizeSpec
import com.upix.ui.theme.Spacing
import com.upix.ui.theme.ValueMono
import com.upix.ui.theme.pressable
import kotlin.math.abs

// 320dp 舞台 / 全屏预览 sheet 的解码目标：默认 192px 缩略图在放大 3-5 倍后
// 明显发糊；按展示区域给更高的解码尺寸（缓存 key 已含尺寸，互不污染）。
private const val STAGE_THUMBNAIL_TARGET_SIZE_PX = 640
private const val PREVIEW_THUMBNAIL_TARGET_SIZE_PX = 960

/**
 * 缩略图解码结果：settled=false 表示解码中（渲染空背景，避免占位文案闪一下），
 * settled=true 且 bitmap=null 表示确实无图，才展示回退文案。
 */
internal data class ThumbnailResult(val bitmap: ImageBitmap?, val settled: Boolean)

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
                .height(if (selectedInputs.isEmpty()) 180.dp else 320.dp)
                // 空态/已选态高度切换（180↔320dp）做尺寸动画，避免舞台跳变。
                .animateContentSize(ExpandSizeSpec),
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
                .clickable(role = Role.Button, onClick = onAddImage),
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
                .clickable(role = Role.Button, onClick = onAddFolder),
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
    val stageResult by produceState(
        initialValue = ThumbnailResult(bitmap = null, settled = false),
        firstSource
    ) {
        value = ThumbnailResult(
            bitmap = firstSource?.let { source ->
                runCatching {
                    decodeQueueThumbnailWithLimit(
                        context = context,
                        source = source,
                        targetSizePx = STAGE_THUMBNAIL_TARGET_SIZE_PX
                    )
                }.getOrNull()
            },
            settled = true
        )
    }

    Box(modifier = Modifier.fillMaxSize()) {
        val result = stageResult
        when {
            // 解码中：安静的 surfaceContainerHigh 占位井（规范：加载态不用动效骨架）。
            !result.settled -> Box(
                modifier = Modifier
                    .fillMaxSize()
                    .background(MaterialTheme.colorScheme.surfaceContainerHigh)
            )
            result.bitmap != null -> Image(
                bitmap = result.bitmap,
                contentDescription = null,
                contentScale = ContentScale.Crop,
                modifier = Modifier.fillMaxSize()
            )
            // 解码失败（极少数损坏源）：安静回退文案，不阻断继续选图。
            else -> Text(
                text = stringResource(R.string.hero_no_preview),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.align(Alignment.Center)
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
        // 混排两种条目：contentType 让 Lazy 复用同类型槽位，key 保持不变。
        items(
            items = selectedInputs,
            key = { source -> source },
            contentType = { "stage-thumb" }
        ) { source ->
            StageThumbnail(
                source = source,
                isCorrupted = source in corruptedSources,
                onDelete = { onRemoveInput(source) },
                onPreview = { onPreviewInput(source) }
            )
        }
        item(key = "stage-add-image", contentType = "stage-action") {
            StageActionTile(
                icon = Icons.Outlined.AddPhotoAlternate,
                contentDescriptionRes = R.string.hero_add_image_cd,
                onClick = onAddImage
            )
        }
        item(key = "stage-add-folder", contentType = "stage-action") {
            StageActionTile(
                icon = Icons.Outlined.CreateNewFolder,
                contentDescriptionRes = R.string.hero_add_folder_cd,
                onClick = onAddFolder
            )
        }
        item(key = "stage-clear", contentType = "stage-action") {
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
    Box(modifier = Modifier.size(72.dp)) {
        ThumbnailCell(
            source = source,
            size = 72.dp,
            persistentWell = true,
            modifier = Modifier.fillMaxSize(),
            onClick = onPreview
        ) {
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
        // 视觉 22dp 圆标，命中区由外层 48dp Box 承担 clickable（MD3 最小触控要求）：
        // clickable 挂在 22dp 内圈会把命中区压回 22dp，48dp 占位必须整体可点。
        Box(
            modifier = Modifier
                .align(Alignment.TopEnd)
                .offset(x = 3.dp, y = (-3).dp)
                .size(48.dp)
                .clip(CircleShape)
                .clickable(role = Role.Button, onClick = onDelete),
            contentAlignment = Alignment.Center
        ) {
            Box(
                modifier = Modifier
                    .size(22.dp)
                    .clip(CircleShape)
                    .background(MaterialTheme.colorScheme.surfaceContainerHighest)
                    .border(1.dp, MaterialTheme.colorScheme.outlineVariant, CircleShape),
                contentAlignment = Alignment.Center
            ) {
                Icon(
                    imageVector = Icons.Outlined.Close,
                    contentDescription = stringResource(R.string.hero_thumbnail_remove_cd),
                    tint = MaterialTheme.colorScheme.onSurface,
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
    // 全出血铺满屏宽：固定 960px 档会被拉伸发糊，解码目标至少取屏幕物理宽度。
    val previewTargetPx = maxOf(
        PREVIEW_THUMBNAIL_TARGET_SIZE_PX,
        context.resources.displayMetrics.widthPixels
    )
    val previewResult by produceState(
        initialValue = ThumbnailResult(bitmap = null, settled = false),
        source
    ) {
        value = ThumbnailResult(
            bitmap = runCatching {
                decodeQueueThumbnailWithLimit(
                    context = context,
                    source = source,
                    targetSizePx = previewTargetPx
                )
            }.getOrNull(),
            settled = true
        )
    }

    ModalBottomSheet(
        onDismissRequest = onDismiss,
        sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true),
        shape = MaterialTheme.shapes.extraLarge,
        containerColor = MaterialTheme.colorScheme.surface
    ) {
        // 照片查看器：图片铺满 sheet 宽，视口高度封顶；双指缩放（1-6x）+
        // 拖动平移 + 双击切换放大。长图（高 > 视口）在 1x 即可上下滑动查看。
        val result = previewResult
        val bitmap = result.bitmap
        var scale by remember(source) { mutableFloatStateOf(1f) }
        var offset by remember(source) { mutableStateOf(Offset.Zero) }
        var viewport by remember(source) { mutableStateOf(IntSize.Zero) }
        var contentSize by remember(source) { mutableStateOf(IntSize.Zero) }

        // 平移夹取：内容缩放尺寸超出视口的范围内才允许偏移。
        fun clampOffset(candidate: Offset): Offset {
            if (bitmap == null || viewport.width == 0 || viewport.height == 0) {
                return Offset.Zero
            }
            val maxX = (((contentSize.width * scale) - viewport.width) / 2f).coerceAtLeast(0f)
            val maxY = (((contentSize.height * scale) - viewport.height) / 2f).coerceAtLeast(0f)
            return Offset(
                x = candidate.x.coerceIn(-maxX, maxX),
                y = candidate.y.coerceIn(-maxY, maxY)
            )
        }

        Column(modifier = Modifier.fillMaxWidth()) {
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = 240.dp, max = 520.dp)
                    .clipToBounds()
                    .background(MaterialTheme.colorScheme.surfaceContainerHighest)
                    .onSizeChanged { viewport = it }
                    .pointerInput(source) {
                        // detectTransformGestures 的按需消费版本：原实现从第一个 move
                        // 起就消费全部事件，sheet 的下滑关闭永远拿不到手势。这里仅当
                        // 「未放大（scale ≤ 1）且单指」时完全不消费，把纯拖拽让渡给
                        // sheet；捏合缩放与放大后的单指平移仍照旧消费，缩放行为不变。
                        val touchSlop = viewConfiguration.touchSlop
                        awaitEachGesture {
                            awaitFirstDown(requireUnconsumed = false)
                            var zoom = 1f
                            var pan = Offset.Zero
                            var pastTouchSlop = false
                            while (true) {
                                val event = awaitPointerEvent()
                                if (event.changes.any { change -> change.isConsumed }) {
                                    break
                                }
                                val pressed = event.changes.filter { change -> change.pressed }
                                if (pressed.isEmpty()) {
                                    break
                                }
                                val multiTouch = pressed.size >= 2
                                // 让渡条件：单指且未放大（1x 下 pan 本就被夹取为 0）。
                                val passThrough = !multiTouch && scale <= 1f
                                val panChange = Offset(
                                    x = pressed.map { change ->
                                        change.position.x - change.previousPosition.x
                                    }.average().toFloat(),
                                    y = pressed.map { change ->
                                        change.position.y - change.previousPosition.y
                                    }.average().toFloat()
                                )
                                val spread = if (multiTouch) {
                                    (pressed[0].position - pressed[1].position).getDistance()
                                } else {
                                    0f
                                }
                                val zoomChange = if (multiTouch) {
                                    val previousSpread = (
                                        pressed[0].previousPosition -
                                            pressed[1].previousPosition
                                        ).getDistance()
                                    if (previousSpread > 0f) spread / previousSpread else 1f
                                } else {
                                    1f
                                }
                                if (!pastTouchSlop) {
                                    zoom *= zoomChange
                                    pan += panChange
                                    // 与 detectTransformGestures 相同的触达判定。
                                    val zoomMotion = abs(1f - zoom) * spread
                                    if (zoomMotion > touchSlop || pan.getDistance() > touchSlop) {
                                        pastTouchSlop = true
                                    }
                                }
                                if (!passThrough) {
                                    // 触达阈值前也保持消费：否则 sheet 的拖拽可能在
                                    // 捏合越过阈值之前先抢占指针（原实现即全程消费）。
                                    if (pastTouchSlop) {
                                        if (zoomChange != 1f) {
                                            scale = (scale * zoomChange).coerceIn(1f, 6f)
                                        }
                                        if (panChange != Offset.Zero) {
                                            offset = clampOffset(offset + panChange)
                                        }
                                    }
                                    event.changes.forEach { change ->
                                        if (change.position != change.previousPosition) {
                                            change.consume()
                                        }
                                    }
                                }
                                // passThrough：不消费也不更新图片状态，sheet 自行决定
                                // 是否把这次拖拽接管为下滑关闭。
                            }
                        }
                    }
                    .pointerInput(source) {
                        detectTapGestures(
                            onDoubleTap = {
                                if (scale > 1f) {
                                    scale = 1f
                                    offset = Offset.Zero
                                } else {
                                    scale = 2.5f
                                    offset = clampOffset(Offset.Zero)
                                }
                            }
                        )
                    },
                contentAlignment = Alignment.Center
            ) {
                when {
                    // 解码中：只留背景，避免「暂无预览」闪现。
                    !result.settled -> Unit
                    bitmap != null -> Image(
                        bitmap = bitmap,
                        contentDescription = null,
                        modifier = Modifier
                            // 无限高测子项：长图拿到完整固有高度布局，居中放入
                            // 视口，溢出由外层 clipToBounds 裁掉。否则父级
                            // heightIn(max) 会把布局压到 520dp，绘制区与平移夹取
                            // 不一致，滑动尽头露出井底色。
                            .layout { measurable, constraints ->
                                val placeable = measurable.measure(
                                    constraints.copy(minHeight = 0, maxHeight = Constraints.Infinity)
                                )
                                val reported = minOf(placeable.height, constraints.maxHeight)
                                layout(constraints.maxWidth, reported) {
                                    // 居中放置：clampOffset 按「内容中心对称 ± 视口」
                                    // 夹取，放置也必须围绕同一中心，否则平移窗口会
                                    // 滑出位图边界（露灰）或永远看不到某端。
                                    placeable.placeRelative(
                                        (constraints.maxWidth - placeable.width) / 2,
                                        (reported - placeable.height) / 2
                                    )
                                }
                            }
                            .onSizeChanged { contentSize = it }
                            .graphicsLayer {
                                scaleX = scale
                                scaleY = scale
                                translationX = offset.x
                                translationY = offset.y
                            },
                        contentScale = ContentScale.FillWidth
                    )
                    else -> Text(
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
                }
            }
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(start = Spacing.xl, end = Spacing.sm, top = Spacing.sm, bottom = Spacing.md),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(Spacing.sm)
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = previewDisplayName(source),
                        style = ValueMono,
                        color = MaterialTheme.colorScheme.onSurface,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                    if (bitmap != null) {
                        Text(
                            text = "${bitmap.width} \u00d7 ${bitmap.height}",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
                IconButton(onClick = onDismiss) {
                    Icon(
                        imageVector = Icons.Outlined.Close,
                        contentDescription = stringResource(R.string.queue_close_cd)
                    )
                }
            }
        }
    }
}

/** 预览信息栏的显示名：URI 取最后段并 URL 解码，路径取文件名，失败回落「image」。 */
private fun previewDisplayName(source: String): String {
    val raw = runCatching { Uri.parse(source.trim()).lastPathSegment }
        .getOrNull()
        ?: source.trim().substringAfterLast('/')
    return runCatching { java.net.URLDecoder.decode(raw, "UTF-8") }
        .getOrDefault(raw)
        .ifBlank { "image" }
}
