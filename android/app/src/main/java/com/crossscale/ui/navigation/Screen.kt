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
        title = "设备",
        iconResId = android.R.drawable.ic_menu_share
    )

    data object Models : AppScreen(
        route = "models",
        title = "模型",
        iconResId = android.R.drawable.ic_menu_gallery
    )

    data object Logs : AppScreen(
        route = "logs",
        title = "日志",
        iconResId = android.R.drawable.ic_menu_info_details
    )

    companion object {
        val bottomNavigationItems = listOf(Home, Devices, Models, Logs)
    }
}
