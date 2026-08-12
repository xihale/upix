package com.crossscale.ui.navigation

sealed class AppScreen(
    val route: String,
    val title: String,
    val iconResId: Int
) {
    data object Home : AppScreen(
        route = "home",
        title = "主页",
        iconResId = android.R.drawable.ic_menu_view
    )

    data object Devices : AppScreen(
        route = "devices",
        title = "连接",
        iconResId = android.R.drawable.ic_menu_share
    )

    data object Settings : AppScreen(
        route = "settings",
        title = "设置",
        iconResId = android.R.drawable.ic_menu_manage
    )

    data object Help : AppScreen(
        route = "help",
        title = "帮助",
        iconResId = android.R.drawable.ic_menu_help
    )

    data object SettingsModels : AppScreen(
        route = "settings/models",
        title = "模型管理",
        iconResId = android.R.drawable.ic_menu_gallery
    )

    data object SettingsLogs : AppScreen(
        route = "settings/logs",
        title = "日志",
        iconResId = android.R.drawable.ic_menu_info_details
    )

    companion object {
        val bottomNavigationItems = listOf(Home, Devices, Settings)
        val allScreens = listOf(Home, Devices, Settings, Help, SettingsModels, SettingsLogs)
    }
}
