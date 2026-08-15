package com.crossscale.ui.screens.home

import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.crossscale.R
import com.crossscale.data.model.ConnectionState
import com.crossscale.data.model.RemoteEndpoint

/**
 * 醒目的处理模式指示条（始终可见，挂在底部操作区内）。
 * 连上 PC → primaryContainer「远程」；未连 → surfaceVariant「本地」；连接中 → tertiaryContainer。
 */
@Composable
internal fun ModeChip(
    connectionState: ConnectionState,
    connectedEndpoint: RemoteEndpoint?
) {
    val colorScheme = MaterialTheme.colorScheme
    val backgroundColor: Color
    val contentColor: Color
    val text: String
    when (connectionState) {
        ConnectionState.CONNECTED -> {
            backgroundColor = colorScheme.primaryContainer
            contentColor = colorScheme.onPrimaryContainer
            val endpoint = connectedEndpoint?.let { "${it.host}:${it.port}" }.orEmpty()
            text = stringResource(R.string.home_mode_remote, endpoint)
        }
        ConnectionState.DISCONNECTED -> {
            backgroundColor = colorScheme.surfaceVariant
            contentColor = colorScheme.onSurfaceVariant
            text = stringResource(R.string.home_mode_local)
        }
        ConnectionState.CONNECTING -> {
            backgroundColor = colorScheme.tertiaryContainer
            contentColor = colorScheme.onTertiaryContainer
            text = stringResource(R.string.home_mode_connecting)
        }
    }

    Surface(
        modifier = Modifier.fillMaxWidth(),
        color = backgroundColor,
        contentColor = contentColor,
        shape = RoundedCornerShape(20.dp)
    ) {
        Text(
            text = text,
            style = MaterialTheme.typography.titleSmall,
            fontWeight = FontWeight.Bold,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 10.dp)
        )
    }
}
