package io.upix.ui.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import io.upix.R
import io.upix.ui.components.SectionHeader
import io.upix.ui.theme.LocalScreenMargin
import io.upix.ui.theme.MaxContentWidth
import io.upix.ui.theme.Spacing

/** 步骤圆形徽章直径。 */
private val StepBadgeSize = 32.dp

/** 步骤间连接线宽度。 */
private val StepConnectorWidth = 2.dp

/**
 * 帮助屏：快速上手步骤的竖向 stepper。
 * 每步为「圆形序号徽章（primaryContainer 调色）+ 内容卡（surfaceContainerLow）」，
 * 徽章间以 outlineVariant 细线相连；宽屏下内容限宽 [MaxContentWidth] 居中。
 * 纯展示屏，无交互与行为。
 */
@Composable
fun HelpScreen() {
    Box(
        modifier = Modifier.fillMaxSize(),
        contentAlignment = Alignment.TopCenter
    ) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .widthIn(max = MaxContentWidth)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = LocalScreenMargin.current)
                .padding(top = Spacing.md, bottom = Spacing.xxl),
            verticalArrangement = Arrangement.spacedBy(Spacing.sm)
        ) {
            SectionHeader(text = stringResource(R.string.help_section_steps))
            // 步骤行之间不额外留白：行内底部留白即步间距，
            // 连接线可穿越留白与下一徽章视觉连续。
            Column {
                val steps = listOf(
                    R.string.help_step_1 to R.string.help_step_1_desc,
                    R.string.help_step_2 to R.string.help_step_2_desc,
                    R.string.help_step_3 to R.string.help_step_3_desc
                )
                steps.forEachIndexed { index, (titleRes, descRes) ->
                    HelpStepItem(
                        number = index + 1,
                        title = stringResource(titleRes),
                        description = stringResource(descRes),
                        showConnector = index != steps.lastIndex
                    )
                }
            }
        }
    }
}

/**
 * 单个步骤行：左侧「徽章 + 连接线」轨道，右侧步骤内容卡。
 * 行高取 IntrinsicSize.Min 使连接线随内容卡高度自动延伸至下一步徽章；
 * 非末步行加底部留白，连接线穿越留白保持视觉连续。
 */
@Composable
private fun HelpStepItem(
    number: Int,
    title: String,
    description: String,
    showConnector: Boolean,
    modifier: Modifier = Modifier
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .height(IntrinsicSize.Min)
            .then(
                if (showConnector) {
                    Modifier.padding(bottom = Spacing.md)
                } else {
                    Modifier
                }
            ),
        horizontalArrangement = Arrangement.spacedBy(Spacing.lg)
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Surface(
                shape = CircleShape,
                color = MaterialTheme.colorScheme.primaryContainer
            ) {
                Box(
                    modifier = Modifier.size(StepBadgeSize),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        text = number.toString(),
                        style = MaterialTheme.typography.titleSmall,
                        color = MaterialTheme.colorScheme.onPrimaryContainer
                    )
                }
            }
            if (showConnector) {
                Box(
                    modifier = Modifier
                        .width(StepConnectorWidth)
                        .fillMaxHeight()
                        .background(MaterialTheme.colorScheme.outlineVariant)
                )
            }
        }
        Surface(
            modifier = Modifier.weight(1f),
            shape = MaterialTheme.shapes.medium,
            color = MaterialTheme.colorScheme.surfaceContainerLow
        ) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(all = Spacing.lg)
            ) {
                Text(
                    text = title,
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.onSurface
                )
                Text(
                    text = description,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(top = Spacing.xs)
                )
            }
        }
    }
}
