package com.crossscale.ui.theme

import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp

/**
 * 统一间距比例（4dp 基网格）。
 * 页面横向边距请用 [LocalScreenMargin]：MD3 布局规范 compact 16dp /
 * medium+ 24dp，随窗口宽度类自适应。
 */
object Spacing {
    val xs = 4.dp
    val sm = 8.dp
    val md = 12.dp
    val lg = 16.dp
    val xl = 20.dp
    val xxl = 24.dp
}

/**
 * 页面内容横向边距：由 app shell 按窗口宽度类提供
 * （compact 16dp / medium+ 24dp，MD3 margin 规范）。
 */
val LocalScreenMargin = staticCompositionLocalOf<Dp> { Spacing.lg }

/** expanded+ 宽屏上正文列的最大宽度（MD3 大屏防拉伸要求）。 */
val MaxContentWidth = 840.dp
