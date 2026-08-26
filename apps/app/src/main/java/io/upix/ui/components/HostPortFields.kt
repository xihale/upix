package io.upix.ui.components

import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue

/** 手动连接默认端口。 */
private const val DEFAULT_MANUAL_CONNECT_PORT = "9527"

/**
 * 手动连接 host/port 表单的共享状态 holder：字段值 + 就地校验（host 非空、
 * 端口 1..65535）。`ConnectionDialog` 等表单宿主各自 `rememberHostPortFields()`
 * 后委托读写与校验，避免各处复制粘贴字段/校验逻辑；不合并对话框本身。
 */
class HostPortFields(
    initialHost: String = "",
    initialPort: String = DEFAULT_MANUAL_CONNECT_PORT
) {
    var host by mutableStateOf(initialHost)
    var port by mutableStateOf(initialPort)
    var hostBlank by mutableStateOf(false)
    var portInvalid by mutableStateOf(false)

    /** host 输入即清错误，保持就地报错语义。 */
    fun onHostChange(value: String) {
        host = value
        hostBlank = false
    }

    /** port 输入即清错误，保持就地报错语义。 */
    fun onPortChange(value: String) {
        port = value
        portInvalid = false
    }

    /**
     * 提交前校验：通过返回 (去空白 host, 去空白 port)，否则置位错误标记并返回 null。
     */
    fun validate(): Pair<String, String>? {
        val trimmedHost = host.trim()
        val portNumber = port.trim().toIntOrNull()
        val portValid = portNumber != null && portNumber in 1..65535
        hostBlank = trimmedHost.isBlank()
        portInvalid = !portValid
        if (trimmedHost.isNotBlank() && portValid) {
            return trimmedHost to port.trim()
        }
        return null
    }
}

/** 表单宿主用：与原 dialog 相同的生命周期（组合内 remember，非 saveable）。 */
@Composable
fun rememberHostPortFields(): HostPortFields =
    remember { HostPortFields() }
