package io.upix.ui.screens.home

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.size
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.Dp
import io.upix.data.thumbnails.decodeQueueThumbnailWithLimit
import io.upix.ui.theme.pressable

/**
 * 队列缩略图单元：HeroArea 缩略行瓦片与队列 sheet 列表头图共用。
 * 统一负责三态渲染——解码中一律不渲染内容（占位井只由 [persistentWell]
 * 开启的修饰符底色承担，且贯穿所有状态；无常驻井的站点在解码期间保持
 * 完全透明，规范：加载态不用动效骨架）、有图 ContentScale.Crop 铺满、
 * 无图回落到调用方的 [fallbackContent]。解码统一走
 * [decodeQueueThumbnailWithLimit] 默认 192px 档，限流与缓存 key 语义单点维护。
 *
 * 站点间真实差异以参数表达：
 * - [onClick]：可点性（舞台瓦片点击进预览；队列行由整卡承担点击）。
 * - [persistentWell]：surfaceContainerHigh 底色是否延续到无图回落态。
 *   舞台瓦片需要常驻井底色才在舞台行上有存在感；队列行回落态刻意保持
 *   透明，让非图条目呈现干净的列表观感。
 */
@Composable
internal fun ThumbnailCell(
    source: String,
    size: Dp,
    persistentWell: Boolean,
    modifier: Modifier = Modifier,
    onClick: (() -> Unit)? = null,
    fallbackContent: @Composable () -> Unit
) {
    val context = LocalContext.current
    val bitmapState by produceState(
        initialValue = ThumbnailResult(bitmap = null, settled = false),
        source
    ) {
        value = ThumbnailResult(
            bitmap = runCatching {
                decodeQueueThumbnailWithLimit(context = context, source = source)
            }.getOrNull(),
            settled = true
        )
    }

    Box(
        modifier = modifier
            .size(size)
            .clip(MaterialTheme.shapes.small)
            // 底色必须在 pressable 之前：pressable 内部追加按压缩放 graphicsLayer，
            // 顺序错了会把井底色卷进缩放范围，按压动效就变了。
            .then(
                if (persistentWell) {
                    Modifier.background(MaterialTheme.colorScheme.surfaceContainerHigh)
                } else {
                    Modifier
                }
            )
            .then(if (onClick != null) Modifier.pressable(onClick = onClick) else Modifier),
        contentAlignment = Alignment.Center
    ) {
        val result = bitmapState
        when {
            // 解码中：一律安静，不渲染任何内容；井底色由 persistentWell
            // 修饰符背景承担并贯穿所有状态，非常驻站点保持完全透明。
            !result.settled -> Unit
            result.bitmap != null -> Image(
                bitmap = result.bitmap,
                contentDescription = null,
                modifier = Modifier.fillMaxSize(),
                contentScale = ContentScale.Crop
            )
            else -> fallbackContent()
        }
    }
}
