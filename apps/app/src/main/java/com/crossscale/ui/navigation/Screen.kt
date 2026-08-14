package com.crossscale.ui.navigation

import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.HelpOutline
import androidx.compose.material.icons.automirrored.outlined.ReceiptLong
import androidx.compose.material.icons.outlined.Devices
import androidx.compose.material.icons.outlined.Home
import androidx.compose.material.icons.outlined.ModelTraining
import androidx.compose.material.icons.outlined.Settings
import androidx.compose.ui.graphics.vector.ImageVector

sealed class AppScreen(
    val route: String,
    val title: String,
    val icon: ImageVector
) {
    data object Home : AppScreen(
        route = "home",
        title = "主页",
        icon = Icons.Outlined.Home
    )

    data object Devices : AppScreen(
        route = "devices",
        title = "连接",
        icon = Icons.Outlined.Devices
    )

    data object Settings : AppScreen(
        route = "settings",
        title = "设置",
        icon = Icons.Outlined.Settings
    )

    data object Help : AppScreen(
        route = "help",
        title = "帮助",
        icon = Icons.AutoMirrored.Outlined.HelpOutline
    )

    data object SettingsModels : AppScreen(
        route = "settings/models",
        title = "模型管理",
        icon = Icons.Outlined.ModelTraining
    )

    data object SettingsLogs : AppScreen(
        route = "settings/logs",
        title = "日志",
        icon = Icons.AutoMirrored.Outlined.ReceiptLong
    )

    companion object {
        val bottomNavigationItems = listOf(Home, Devices, Settings)
        val allScreens = listOf(Home, Devices, Settings, Help, SettingsModels, SettingsLogs)
    }
}
