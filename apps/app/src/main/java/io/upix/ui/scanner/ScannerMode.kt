package com.upix.ui.scanner

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

/**
 * 扫码模式全局开关：扫码界面为全屏相机体验，
 * 宿主 Scaffold 据此隐藏底部导航栏，避免扫码页下方残留导航栏。
 */
object ScannerMode {
    var isActive by mutableStateOf(false)
        private set

    internal fun enter() {
        isActive = true
    }

    internal fun exit() {
        isActive = false
    }
}
