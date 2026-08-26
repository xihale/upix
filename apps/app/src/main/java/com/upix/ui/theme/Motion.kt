package com.upix.ui.theme

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
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp

/**
 * 统一动效规范（MD3 转场 token）：
 * - 转场按 MD3 规范：进入 emphasized-decelerate（400ms）、退出
 *   emphasized-accelerate（200ms），曲线取官方值；组件内 spring 动效
 *   交给 Expressive 主题的 MotionScheme。
 * - 禁止 ease-in 进入、禁止 scale(0) 起始；只动 transform/alpha。
 * - 无循环/装饰性动效（无 shimmer/脉冲点/渐变动画），加载 = 安静占位 + 确定性进度。
 * - 本文件是自定义动效 token 的规范来源（docs/DESIGN_LANGUAGE.md §7）；组件级
 *   spring 动效继承 Expressive MotionScheme，不得逐组件对抗默认值。
 * - 已知限制：Compose 无 prefers-reduced-motion 等价系统 API，全局降级暂不可做。
 */

/** MD3 emphasized decelerate（进入屏幕）。 */
val EmphasizedDecelerateEasing = CubicBezierEasing(0.05f, 0.7f, 0.1f, 1f)

/** MD3 emphasized accelerate（退出屏幕）。 */
val EmphasizedAccelerateEasing = CubicBezierEasing(0.3f, 0f, 0.8f, 0.15f)

const val PressTween = 100
const val EnterTween = 140
const val ExpandTween = 180

/** MD3 转场时长：进入 400ms（decelerate）、退出 200ms（accelerate）。 */
const val NavEnterTween = 400
const val NavExitTween = 200

/** 按压缩放（100ms，decelerate）。 */
val PressMotionSpec = tween<Float>(PressTween, easing = EmphasizedDecelerateEasing)

/** 进入/退出 alpha（140ms）。 */
val EnterMotionSpec = tween<Float>(EnterTween, easing = EmphasizedDecelerateEasing)

/** 折叠区/出现消失的高度展开（180ms）。 */
val ExpandSizeSpec = tween<IntSize>(ExpandTween, easing = EmphasizedDecelerateEasing)

/** AnimatedVisibility 进入/退出的尺寸 spec（140ms，与 fade 同步）。 */
val EnterSizeSpec = tween<IntSize>(EnterTween, easing = EmphasizedDecelerateEasing)

/** NavHost 目的地进入：纯 fade（400ms emphasized-decelerate），不做位移。 */
val NavEnterTransition: EnterTransition = fadeIn(tween(NavEnterTween, easing = EmphasizedDecelerateEasing))

/** NavHost 目的地退出：纯 fade（200ms emphasized-accelerate），不做位移。 */
val NavExitTransition: ExitTransition = fadeOut(tween(NavExitTween, easing = EmphasizedAccelerateEasing))

/** 返回键进入（pop）：与正向进入一致（400ms decelerate）。 */
val NavPopEnterTransition: EnterTransition = NavEnterTransition

/** 返回键退出（pop）：与正向退出一致（200ms accelerate）。 */
val NavPopExitTransition: ExitTransition = NavExitTransition

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
