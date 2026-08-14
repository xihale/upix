package com.crossscale.ui.navigation

import androidx.annotation.StringRes
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.HelpOutline
import androidx.compose.material.icons.automirrored.outlined.ReceiptLong
import androidx.compose.material.icons.outlined.Devices
import androidx.compose.material.icons.outlined.Home
import androidx.compose.material.icons.outlined.ModelTraining
import androidx.compose.material.icons.outlined.Settings
import androidx.compose.ui.graphics.vector.ImageVector
import com.crossscale.R

sealed class AppScreen(
    val route: String,
    @StringRes val titleRes: Int,
    val icon: ImageVector
) {
    data object Home : AppScreen(
        route = "home",
        titleRes = R.string.nav_home,
        icon = Icons.Outlined.Home
    )

    data object Devices : AppScreen(
        route = "devices",
        titleRes = R.string.nav_connection,
        icon = Icons.Outlined.Devices
    )

    data object Settings : AppScreen(
        route = "settings",
        titleRes = R.string.common_settings,
        icon = Icons.Outlined.Settings
    )

    data object Help : AppScreen(
        route = "help",
        titleRes = R.string.nav_help,
        icon = Icons.AutoMirrored.Outlined.HelpOutline
    )

    data object SettingsModels : AppScreen(
        route = "settings/models",
        titleRes = R.string.common_model_management,
        icon = Icons.Outlined.ModelTraining
    )

    data object SettingsLogs : AppScreen(
        route = "settings/logs",
        titleRes = R.string.common_logs,
        icon = Icons.AutoMirrored.Outlined.ReceiptLong
    )

    companion object {
        val bottomNavigationItems = listOf(Home, Devices, Settings)
        val allScreens = listOf(Home, Devices, Settings, Help, SettingsModels, SettingsLogs)
    }
}
