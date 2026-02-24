package com.crossscale.ui.theme

import androidx.compose.runtime.Composable
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp

object BorderlessTokens {
    @get:Composable
    val ScreenPaddingHorizontal: Dp
        get() = when (widthClass()) {
            BorderlessWidthClass.COMPACT -> 20.dp
            BorderlessWidthClass.MEDIUM -> 28.dp
            BorderlessWidthClass.EXPANDED -> 36.dp
        }

    @get:Composable
    val ScreenPaddingVertical: Dp
        get() = when (widthClass()) {
            BorderlessWidthClass.COMPACT -> 12.dp
            BorderlessWidthClass.MEDIUM -> 16.dp
            BorderlessWidthClass.EXPANDED -> 20.dp
        }

    @get:Composable
    val SectionSpacing: Dp
        get() = when (widthClass()) {
            BorderlessWidthClass.COMPACT -> 10.dp
            BorderlessWidthClass.MEDIUM -> 12.dp
            BorderlessWidthClass.EXPANDED -> 14.dp
        }

    @get:Composable
    val SectionCompactSpacing: Dp
        get() = when (widthClass()) {
            BorderlessWidthClass.COMPACT -> 8.dp
            BorderlessWidthClass.MEDIUM -> 10.dp
            BorderlessWidthClass.EXPANDED -> 12.dp
        }

    @get:Composable
    val ListItemSpacing: Dp
        get() = when (widthClass()) {
            BorderlessWidthClass.COMPACT -> 2.dp
            BorderlessWidthClass.MEDIUM -> 4.dp
            BorderlessWidthClass.EXPANDED -> 6.dp
        }

    @get:Composable
    val ListItemVerticalPadding: Dp
        get() = when (widthClass()) {
            BorderlessWidthClass.COMPACT -> 10.dp
            BorderlessWidthClass.MEDIUM -> 12.dp
            BorderlessWidthClass.EXPANDED -> 14.dp
        }

    @get:Composable
    val RowVerticalPadding: Dp
        get() = when (widthClass()) {
            BorderlessWidthClass.COMPACT -> 12.dp
            BorderlessWidthClass.MEDIUM -> 14.dp
            BorderlessWidthClass.EXPANDED -> 16.dp
        }

    @get:Composable
    val InteractiveRowMinHeight: Dp
        get() = when (widthClass()) {
            BorderlessWidthClass.COMPACT -> 52.dp
            BorderlessWidthClass.MEDIUM -> 56.dp
            BorderlessWidthClass.EXPANDED -> 60.dp
        }

    const val DividerStrongAlpha = 0.20f
    const val DividerMediumAlpha = 0.15f
    const val DividerSubtleAlpha = 0.12f
    const val HandleAlpha = 0.45f

    val SectionTitleWeight = FontWeight.SemiBold

    @Composable
    private fun widthClass(): BorderlessWidthClass {
        val widthDp = LocalConfiguration.current.screenWidthDp
        return when {
            widthDp >= EXPANDED_MIN_WIDTH_DP -> BorderlessWidthClass.EXPANDED
            widthDp >= MEDIUM_MIN_WIDTH_DP -> BorderlessWidthClass.MEDIUM
            else -> BorderlessWidthClass.COMPACT
        }
    }
}

private enum class BorderlessWidthClass {
    COMPACT,
    MEDIUM,
    EXPANDED
}

private const val MEDIUM_MIN_WIDTH_DP = 600
private const val EXPANDED_MIN_WIDTH_DP = 840
