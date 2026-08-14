package com.crossscale.ui.components

import androidx.compose.material3.Icon
import androidx.compose.material3.LocalRippleConfiguration
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.getValue
import androidx.navigation.NavDestination
import androidx.navigation.NavDestination.Companion.hierarchy
import androidx.navigation.NavController
import androidx.navigation.compose.currentBackStackEntryAsState
import com.crossscale.ui.navigation.AppScreen

@Composable
fun BottomNavigationBar(navController: NavController) {
    val items = AppScreen.bottomNavigationItems

    CompositionLocalProvider(LocalRippleConfiguration provides null) {
        NavigationBar {
        val navBackStackEntry by navController.currentBackStackEntryAsState()
        val currentDestination = navBackStackEntry?.destination

        items.forEach { screen ->
            val selected = isScreenSelected(currentDestination = currentDestination, screen = screen)

            NavigationBarItem(
                icon = { Icon(screen.icon, contentDescription = screen.title) },
                label = { Text(screen.title) },
                selected = selected,
                onClick = {
                    if (selected) {
                        return@NavigationBarItem
                    }
                    navController.navigate(screen.route) {
                        navController.graph.startDestinationRoute?.let { route ->
                            popUpTo(route) {
                                saveState = true
                            }
                        }
                        launchSingleTop = true
                        restoreState = true
                    }
                }
            )
        }
        }
    }
}

private fun isScreenSelected(currentDestination: NavDestination?, screen: AppScreen): Boolean {
    if (currentDestination == null) {
        return false
    }
    if (currentDestination.hierarchy.any { destination -> destination.route == screen.route }) {
        return true
    }
    return when (screen) {
        AppScreen.Settings -> {
            currentDestination.hierarchy.any { destination ->
                val route = destination.route ?: return@any false
                route == AppScreen.Help.route || route.startsWith("${AppScreen.Settings.route}/")
            }
        }

        else -> false
    }
}
