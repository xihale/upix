package com.crossscale.ui.components

import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Text
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.PressInteraction
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.navigation.NavDestination
import androidx.navigation.NavDestination.Companion.hierarchy
import androidx.compose.ui.res.painterResource
import androidx.navigation.NavController
import androidx.navigation.compose.currentBackStackEntryAsState
import com.crossscale.ui.navigation.AppScreen

@Composable
fun BottomNavigationBar(navController: NavController) {
    val items = AppScreen.bottomNavigationItems
    var pendingSelectedRoute by remember { mutableStateOf<String?>(null) }

    NavigationBar {
        val navBackStackEntry by navController.currentBackStackEntryAsState()
        val currentDestination = navBackStackEntry?.destination

        LaunchedEffect(currentDestination, pendingSelectedRoute) {
            val pendingRoute = pendingSelectedRoute ?: return@LaunchedEffect
            val pendingScreen = items.firstOrNull { screen ->
                screen.route == pendingRoute
            } ?: run {
                pendingSelectedRoute = null
                return@LaunchedEffect
            }
            if (isScreenSelected(currentDestination = currentDestination, screen = pendingScreen)) {
                pendingSelectedRoute = null
            }
        }

        items.forEach { screen ->
            val interactionSource = remember(screen.route) { MutableInteractionSource() }
            val actuallySelected = isScreenSelected(currentDestination = currentDestination, screen = screen)
            val selected = actuallySelected || pendingSelectedRoute == screen.route

            LaunchedEffect(interactionSource, screen.route) {
                interactionSource.interactions.collect { interaction ->
                    if (interaction is PressInteraction.Press) {
                        pendingSelectedRoute = screen.route
                    }
                }
            }

            NavigationBarItem(
                icon = { Icon(painterResource(id = screen.iconResId), contentDescription = screen.title) },
                label = { Text(screen.title) },
                selected = selected,
                interactionSource = interactionSource,
                onClick = {
                    if (actuallySelected) {
                        pendingSelectedRoute = null
                        return@NavigationBarItem
                    }
                    pendingSelectedRoute = screen.route
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
