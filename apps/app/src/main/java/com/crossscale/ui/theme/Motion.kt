package com.crossscale.ui.theme

import androidx.compose.animation.EnterTransition
import androidx.compose.animation.ExitTransition
import androidx.compose.animation.core.CubicBezierEasing
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.LocalIndication
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp

/**
 * 统一动效规范（克制、可预期）：
 * - 时长一律 <300ms；进入/退出统一 ease-out（EmphasizedEasing）。
 * - 禁止 ease-in 进入、禁止 scale(0) 起始；只动 transform/alpha。
 * - 已知限制：Compose 无 prefers-reduced-motion 等价系统 API，全局降级暂不可做。
 */

/** 统一 ease-out 曲线。 */
val EmphasizedEasing = CubicBezierEasing(0.23f, 1f, 0.32f, 1f)

const val PressTween = 120
const val EnterTween = 200
const val ExpandTween = 250

/** NavHost 目的地退场 fade（90ms，快进快出，不与进场抢注意力）。 */
const val NavExitTween = 90

/** 按压缩放（120ms）。 */
val PressMotionSpec = tween<Float>(PressTween, easing = EmphasizedEasing)

/** 进入/退出 alpha（200ms）。 */
val EnterMotionSpec = tween<Float>(EnterTween, easing = EmphasizedEasing)

/** 折叠区/出现消失的高度展开（250ms）。 */
val ExpandSizeSpec = tween<IntSize>(ExpandTween, easing = EmphasizedEasing)

/** AnimatedVisibility 进入/退出的尺寸 spec（200ms，与 fade 同步）。 */
val EnterSizeSpec = tween<IntSize>(EnterTween, easing = EmphasizedEasing)

/** AnimatedContent 换内容的位移 spec（200ms）。 */
val EnterOffsetSpec = tween<IntOffset>(EnterTween, easing = EmphasizedEasing)

/** NavHost 目的地进入：fade-in（200ms，EmphasizedEasing），替代默认 700ms fade。 */
val NavEnterTransition: EnterTransition = fadeIn(tween(EnterTween, easing = EmphasizedEasing))

/** NavHost 目的地退出：fade-out（90ms）。 */
val NavExitTransition: ExitTransition = fadeOut(tween(NavExitTween))

/**
 * 给定 interactionSource，按下时缩到 0.97（graphicsLayer，仅 transform）。
 * 用于已有自带交互的 M3 组件（Button 等），把同一个 source 传给组件即可。
 */
@Composable
fun Modifier.pressScale(interactionSource: MutableInteractionSource): Modifier {
    val pressed by interactionSource.collectIsPressedAsState()
    val scale by animateFloatAsState(
        targetValue = if (pressed) 0.97f else 1f,
        animationSpec = PressMotionSpec,
        label = "pressScale"
    )
    return graphicsLayer {
        scaleX = scale
        scaleY = scale
    }
}

/**
 * clickable 包装：内部创建 interactionSource，涟漪（LocalIndication）+ 按压缩放共用
 * 同一 source，替换裸 clickable 时不丢涟漪、语义不变。
 */
@Composable
fun Modifier.pressable(
    enabled: Boolean = true,
    onClick: () -> Unit
): Modifier {
    val interactionSource = remember { MutableInteractionSource() }
    return this
        .pressScale(interactionSource)
        .clickable(
            interactionSource = interactionSource,
            indication = LocalIndication.current,
            enabled = enabled,
            onClick = onClick
        )
}

/**
 * 进度条数值平滑（300ms 线性）：配合 lambda 版
 * `LinearProgressIndicator(progress = { smoothProgress(value) })` 使用。
 */
@Composable
fun smoothProgress(target: Float): Float {
    val animated by animateFloatAsState(
        targetValue = target,
        animationSpec = tween(300, easing = LinearEasing),
        label = "smoothProgress"
    )
    return animated
}
