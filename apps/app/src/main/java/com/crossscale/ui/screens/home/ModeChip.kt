package com.crossscale.ui.screens.home

import androidx.compose.animation.animateColorAsState
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import com.crossscale.R
import com.crossscale.data.model.ConnectionState
import com.crossscale.data.model.RemoteEndpoint
import com.crossscale.ui.theme.Spacing

/**
 * 处理模式分段控件（展示当前连接态，随 DeviceViewModel 自动流转）：
 * 本地 / 连接中 / 远程 三段等宽，选中段 primary 底 onPrimary 字。
 * 模式由连接状态驱动，无手动切换回调，故仅作指示器。
 */
@Composable
internal fun ModeSegment(
    connectionState: ConnectionState,
    connectedEndpoint: RemoteEndpoint?
) {
    val colorScheme = MaterialTheme.colorScheme
    val segmentShape = MaterialTheme.shapes.small

    Column(verticalArrangement = Arrangement.spacedBy(Spacing.xs)) {
        Surface(
            modifier = Modifier.fillMaxWidth(),
            shape = MaterialTheme.shapes.medium,
            color = colorScheme.surfaceContainerHigh
        ) {
            Row(modifier = Modifier.padding(Spacing.xs)) {
                ModeSegmentItem(
                    label = stringResource(R.string.config_mode_local),
                    selected = connectionState == ConnectionState.DISCONNECTED,
                    modifier = Modifier.weight(1f),
                    segmentShape = segmentShape
                )
                ModeSegmentItem(
                    label = stringResource(R.string.config_mode_connecting),
                    selected = connectionState == ConnectionState.CONNECTING,
                    modifier = Modifier.weight(1f),
                    segmentShape = segmentShape
                )
                ModeSegmentItem(
                    label = stringResource(R.string.config_mode_remote),
                    selected = connectionState == ConnectionState.CONNECTED,
                    modifier = Modifier.weight(1f),
                    segmentShape = segmentShape
                )
            }
        }
        if (connectionState == ConnectionState.CONNECTED) {
            val endpoint = connectedEndpoint?.let { "${it.host}:${it.port}" }.orEmpty()
            Text(
                text = stringResource(R.string.config_mode_endpoint, endpoint),
                style = MaterialTheme.typography.labelSmall,
                color = colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
        }
    }
}

@Composable
private fun ModeSegmentItem(
    label: String,
    selected: Boolean,
    modifier: Modifier = Modifier,
    segmentShape: androidx.compose.ui.graphics.Shape
) {
    val container by animateColorAsState(
        targetValue = if (selected) {
            MaterialTheme.colorScheme.primary
        } else {
            MaterialTheme.colorScheme.surfaceContainerHigh
        },
        label = "modeSegmentContainer"
    )
    val content by animateColorAsState(
        targetValue = if (selected) {
            MaterialTheme.colorScheme.onPrimary
        } else {
            MaterialTheme.colorScheme.onSurfaceVariant
        },
        label = "modeSegmentContent"
    )
    androidx.compose.foundation.layout.Box(
        modifier = modifier
            .background(color = container, shape = segmentShape)
            .padding(vertical = Spacing.sm),
        contentAlignment = Alignment.Center
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.labelLarge,
            color = content,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis
        )
    }
}
