package com.crossscale.ui.screens.home

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
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
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.blur
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.unit.dp
import com.crossscale.data.thumbnails.decodeQueueThumbnailWithLimit
import com.crossscale.ui.theme.BorderlessTokens

@Composable
internal fun HeroArea(
    selectedInputs: List<String>,
    corruptedSources: Set<String>,
    onAddImage: () -> Unit,
    onAddFolder: () -> Unit,
    onClearInput: () -> Unit,
    onRemoveInput: (String) -> Unit,
    onPreviewInput: (String) -> Unit
) {
    Surface(
        modifier = Modifier
            .fillMaxWidth()
            .height(280.dp)
            .clip(RoundedCornerShape(26.dp))
            .combinedClickable(
                onClick = onAddImage,
                onLongClick = onAddFolder
            ),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.26f)
    ) {
        if (selectedInputs.isEmpty()) {
            EmptyHero()
        } else {
            SelectedHero(
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

@Composable
private fun EmptyHero() {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(18.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Box(
            modifier = Modifier
                .size(84.dp)
                .clip(CircleShape)
                .background(
                    brush = Brush.linearGradient(
                        colors = listOf(
                            MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.95f),
                            MaterialTheme.colorScheme.secondaryContainer.copy(alpha = 0.85f)
                        )
                    )
                ),
            contentAlignment = Alignment.Center
        ) {
            Icon(
                painter = painterResource(id = android.R.drawable.ic_input_add),
                contentDescription = null,
                tint = Color.White,
                modifier = Modifier.size(36.dp)
            )
        }
        Spacer(modifier = Modifier.height(14.dp))
        Text(
            text = "添加图片或文件夹",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }
}

@Composable
private fun SelectedHero(
    selectedInputs: List<String>,
    corruptedSources: Set<String>,
    onAddImage: () -> Unit,
    onAddFolder: () -> Unit,
    onClearInput: () -> Unit,
    onRemoveInput: (String) -> Unit,
    onPreviewInput: (String) -> Unit
) {
    val context = LocalContext.current
    val firstSource = selectedInputs.firstOrNull()
    val bgBitmap by produceState<ImageBitmap?>(initialValue = null, firstSource) {
        value = firstSource?.let { source ->
            decodeQueueThumbnailWithLimit(context = context, source = source)
        }
    }

    Box(modifier = Modifier.fillMaxSize()) {
        if (bgBitmap != null) {
            Image(
                bitmap = bgBitmap!!,
                contentDescription = null,
                contentScale = ContentScale.Crop,
                modifier = Modifier
                    .fillMaxSize()
                    .blur(22.dp)
            )
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .background(MaterialTheme.colorScheme.surface.copy(alpha = 0.62f))
            )
        }

        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(12.dp),
            verticalArrangement = Arrangement.SpaceBetween
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = "已选择 ${selectedInputs.size} 项",
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = BorderlessTokens.SectionTitleWeight
                )
                Text(
                    text = "继续添加",
                    style = MaterialTheme.typography.labelLarge,
                    color = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.combinedClickable(onClick = onAddImage, onLongClick = onAddFolder)
                )
            }

            LazyRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                items(items = selectedInputs, key = { item -> item }) { source ->
                    HeroThumbnail(
                        source = source,
                        isCorrupted = source in corruptedSources,
                        onDelete = { onRemoveInput(source) },
                        onPreview = { onPreviewInput(source) }
                    )
                }
            }
        }

        Surface(
            modifier = Modifier
                .align(Alignment.BottomEnd)
                .padding(12.dp)
                .size(34.dp)
                .clickable(onClick = onClearInput),
            shape = CircleShape,
            color = MaterialTheme.colorScheme.surface
        ) {
            Box(contentAlignment = Alignment.Center) {
                Icon(
                    painter = painterResource(id = android.R.drawable.ic_menu_close_clear_cancel),
                    contentDescription = null,
                    modifier = Modifier.size(16.dp),
                    tint = MaterialTheme.colorScheme.onSurface
                )
            }
        }
    }
}

@Composable
private fun HeroThumbnail(
    source: String,
    isCorrupted: Boolean,
    onDelete: () -> Unit,
    onPreview: () -> Unit
) {
    val context = LocalContext.current
    val bitmapState by produceState<ImageBitmap?>(initialValue = null, source) {
        value = decodeQueueThumbnailWithLimit(context = context, source = source)
    }

    Box(
        modifier = Modifier
            .size(width = 96.dp, height = 116.dp)
            .clip(RoundedCornerShape(14.dp))
            .background(MaterialTheme.colorScheme.surface)
            .combinedClickable(onClick = onDelete, onLongClick = onPreview),
        contentAlignment = Alignment.Center
    ) {
        val imageBitmap = bitmapState
        if (imageBitmap != null) {
            Image(
                bitmap = imageBitmap,
                contentDescription = null,
                modifier = Modifier.fillMaxSize(),
                contentScale = ContentScale.Crop
            )
        } else {
            Text(
                text = if (isCorrupted) "损坏" else "预览",
                style = MaterialTheme.typography.bodySmall,
                color = if (isCorrupted) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface
            )
        }
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
        value = decodeQueueThumbnailWithLimit(context = context, source = source)
    }
    val previewTitle = remember(source) {
        source.substringAfterLast('/').ifBlank {
            source.takeLast(28).ifBlank { "预览" }
        }
    }

    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = BorderlessTokens.ScreenPaddingHorizontal),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Text(
                text = "预览 · $previewTitle",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = BorderlessTokens.SectionTitleWeight
            )
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = 220.dp, max = 520.dp)
                    .clip(RoundedCornerShape(18.dp))
                    .background(MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.3f)),
                contentAlignment = Alignment.Center
            ) {
                val bitmap = previewBitmap
                if (bitmap == null) {
                    Text(
                        text = if (isCorrupted) "图片损坏" else "暂无可预览图像",
                        style = MaterialTheme.typography.bodyMedium,
                        color = if (isCorrupted) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant
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
            Text(
                text = "长按缩略图可预览，单击缩略图可删除",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Spacer(modifier = Modifier.height(14.dp))
        }
    }
}
