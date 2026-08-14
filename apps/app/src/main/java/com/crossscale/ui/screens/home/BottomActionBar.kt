package com.crossscale.ui.screens.home

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.KeyboardArrowUp
import androidx.compose.material.icons.outlined.Queue
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.unit.dp
import com.crossscale.data.model.ConnectionState
import com.crossscale.data.model.RemoteEndpoint
import com.crossscale.ui.theme.BorderlessTokens

@Composable
internal fun BottomActionBar(
    modifier: Modifier = Modifier,
    isProcessing: Boolean,
    pendingQueueCount: Int,
    failedQueueCount: Int,
    hasBackupAvailable: Boolean = false,
    connectionState: ConnectionState,
    connectedEndpoint: RemoteEndpoint?,
    onShowQueue: () -> Unit,
    onStart: () -> Unit,
    onRestore: () -> Unit = {},
    onCancel: () -> Unit
) {
    Surface(modifier = modifier.fillMaxWidth(), color = MaterialTheme.colorScheme.background) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 10.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            ModeChip(
                connectionState = connectionState,
                connectedEndpoint = connectedEndpoint
            )

            QueueDockEntry(
                pendingQueueCount = pendingQueueCount,
                failedQueueCount = failedQueueCount,
                onShowQueue = onShowQueue
            )

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(10.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                if (hasBackupAvailable && !isProcessing) {
                    Button(
                        onClick = onRestore,
                        modifier = Modifier
                            .weight(1f)
                            .height(52.dp),
                        shape = CircleShape,
                        colors = ButtonDefaults.buttonColors(
                            containerColor = MaterialTheme.colorScheme.tertiaryContainer,
                            contentColor = MaterialTheme.colorScheme.onTertiaryContainer
                        )
                    ) {
                        Text(
                            "恢复备份",
                            fontWeight = BorderlessTokens.SectionTitleWeight
                        )
                    }
                }

                Button(
                    onClick = onStart,
                    modifier = Modifier
                        .weight(1f)
                        .height(52.dp),
                    shape = CircleShape,
                    colors = ButtonDefaults.buttonColors(
                        containerColor = MaterialTheme.colorScheme.primaryContainer,
                        contentColor = MaterialTheme.colorScheme.onPrimaryContainer
                    )
                ) {
                    Text(
                        if (isProcessing) "继续队列" else "开始超分",
                        fontWeight = BorderlessTokens.SectionTitleWeight
                    )
                }

                if (isProcessing) {
                    OutlinedButton(
                        onClick = onCancel,
                        modifier = Modifier
                            .weight(0.68f)
                            .height(52.dp),
                        shape = CircleShape
                    ) {
                        Text("停止")
                    }
                }
            }

            Spacer(modifier = Modifier.height(4.dp))
        }
    }
}

@Composable
private fun QueueDockEntry(
    pendingQueueCount: Int,
    failedQueueCount: Int,
    onShowQueue: () -> Unit
) {
    val interactionSource = remember { MutableInteractionSource() }

    Surface(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(18.dp))
            .clickable(
                interactionSource = interactionSource,
                indication = null,
                onClick = onShowQueue
            ),
        shape = RoundedCornerShape(18.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.3f)
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 14.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            Box(
                modifier = Modifier
                    .size(28.dp)
                    .clip(CircleShape)
                    .background(MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.85f)),
                contentAlignment = Alignment.Center
            ) {
                Icon(
                    imageVector = Icons.Outlined.Queue,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.onPrimaryContainer,
                    modifier = Modifier.size(15.dp)
                )
            }

            Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(
                    text = "队列",
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = BorderlessTokens.SectionTitleWeight
                )
                Text(
                    text = "待处理 $pendingQueueCount · 失败 $failedQueueCount",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }

            QueueStatusBadge(pendingQueueCount = pendingQueueCount, failedQueueCount = failedQueueCount)

            Icon(
                imageVector = Icons.Outlined.KeyboardArrowUp,
                contentDescription = "打开队列",
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.size(16.dp)
            )
        }
    }
}

@Composable
private fun QueueStatusBadge(
    pendingQueueCount: Int,
    failedQueueCount: Int
) {
    val label = when {
        failedQueueCount > 0 -> "${failedQueueCount} 失败"
        pendingQueueCount > 0 -> "${pendingQueueCount} 待处理"
        else -> "空"
    }
    val backgroundColor = when {
        failedQueueCount > 0 -> MaterialTheme.colorScheme.errorContainer
        pendingQueueCount > 0 -> MaterialTheme.colorScheme.secondaryContainer
        else -> MaterialTheme.colorScheme.surface
    }
    val contentColor = when {
        failedQueueCount > 0 -> MaterialTheme.colorScheme.onErrorContainer
        pendingQueueCount > 0 -> MaterialTheme.colorScheme.onSecondaryContainer
        else -> MaterialTheme.colorScheme.onSurfaceVariant
    }

    Surface(shape = CircleShape, color = backgroundColor) {
        Text(
            text = label,
            style = MaterialTheme.typography.labelSmall,
            color = contentColor,
            modifier = Modifier.padding(horizontal = 10.dp, vertical = 4.dp)
        )
    }
}
