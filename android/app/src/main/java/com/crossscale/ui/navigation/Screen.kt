package com.crossscale.ui.navigation

sealed class AppScreen(
    val route: String,
    val title: String,
    val guide: String,
    val iconResId: Int
) {
    data object Home : AppScreen(
        route = "home",
        title = "主页",
        guide = "按步骤完成输入、模型、执行与队列。",
        iconResId = android.R.drawable.ic_menu_view
    )

    data object Devices : AppScreen(
        route = "devices",
        title = "设备",
        guide = "先自动发现，再扫码或手动连接。",
        iconResId = android.R.drawable.ic_menu_share
    )

    data object Models : AppScreen(
        route = "models",
        title = "模型",
        guide = "先选一个默认模型，再按需管理。",
        iconResId = android.R.drawable.ic_menu_gallery
    )

    data object Logs : AppScreen(
        route = "logs",
        title = "日志",
        guide = "先筛选级别，再用关键字定位问题。",
        iconResId = android.R.drawable.ic_menu_info_details
    )

    companion object {
        val bottomNavigationItems = listOf(Home, Devices, Models, Logs)
    }
}
