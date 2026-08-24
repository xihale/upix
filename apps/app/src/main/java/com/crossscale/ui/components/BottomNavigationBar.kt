package com.crossscale.ui.components

import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.res.stringResource
import androidx.navigation.NavController
import androidx.navigation.compose.currentBackStackEntryAsState
import com.crossscale.ui.navigation.AppScreen

@Composable
fun BottomNavigationBar(navController: NavController) {
    val items = AppScreen.bottomNavigationItems

    NavigationBar(containerColor = MaterialTheme.colorScheme.surfaceContainer) {
        val navBackStackEntry by navController.currentBackStackEntryAsState()
        val currentDestination = navBackStackEntry?.destination

        items.forEach { screen ->
            val selected = isScreenSelected(currentDestination = currentDestination, screen = screen)
            val title = stringResource(screen.titleRes)

            NavigationBarItem(
                icon = {
                    // label 已朗读，图标不再带 contentDescription，避免 TalkBack 双报。
                    Icon(
                        imageVector = if (selected) screen.selectedIcon else screen.icon,
                        contentDescription = null
                    )
                },
                label = { Text(title) },
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
