package io.upix.ui.components

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationDrawerItem
import androidx.compose.material3.NavigationRail
import androidx.compose.material3.NavigationRailItem
import androidx.compose.material3.PermanentDrawerSheet
import androidx.compose.material3.PermanentNavigationDrawer
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.navigation.NavDestination
import androidx.navigation.NavDestination.Companion.hierarchy
import androidx.navigation.NavController
import androidx.navigation.compose.currentBackStackEntryAsState
import io.upix.ui.navigation.AppScreen
import io.upix.ui.theme.Spacing

/**
 * MD3 adaptive 导航：compact 用 NavigationBar（见 [BottomNavigationBar]），
 * medium 用 NavigationRail，expanded+ 用 PermanentNavigationDrawer。
 * 选中态判定、目的地切换、选中/未选中图标选择三处共享（settings 子路由、
 * help 层级归属 Settings）。
 */

/** 底部导航/rail/drawer 共享的选中态判定。 */
internal fun isScreenSelected(currentDestination: NavDestination?, screen: AppScreen): Boolean {
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

/** 三种导航形态共用的图标选择：选中态用填充变体。 */
internal fun AppScreen.iconFor(selected: Boolean) = if (selected) selectedIcon else icon

/** 目的地切换：三种导航形态共享同一 pop/save/restore 策略。 */
internal fun navigateToTab(navController: NavController, screen: AppScreen) {
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

@Composable
internal fun currentDestinationOf(navController: NavController): NavDestination? {
    val navBackStackEntry by navController.currentBackStackEntryAsState()
    return navBackStackEntry?.destination
}

/** medium（600–839dp）：侧边导航栏。 */
@Composable
fun AdaptiveNavigationRail(navController: NavController) {
    val items = AppScreen.bottomNavigationItems
    val currentDestination = currentDestinationOf(navController)

    NavigationRail(containerColor = MaterialTheme.colorScheme.surfaceContainer) {
        items.forEach { screen ->
            val selected = isScreenSelected(currentDestination, screen)
            val title = stringResource(screen.titleRes)
            NavigationRailItem(
                // label 已朗读，图标不带 contentDescription，避免 TalkBack 双报。
                icon = {
                    Icon(
                        imageVector = screen.iconFor(selected),
                        contentDescription = null
                    )
                },
                label = { Text(title) },
                selected = selected,
                onClick = {
                    if (!selected) {
                        navigateToTab(navController, screen)
                    }
                }
            )
        }
    }
}

/** expanded+（840dp+）：常驻侧边抽屉，content 为应用主体（NavHost）。 */
@Composable
fun AppNavigationDrawer(
    navController: NavController,
    content: @Composable () -> Unit
) {
    val items = AppScreen.bottomNavigationItems
    val currentDestination = currentDestinationOf(navController)

    PermanentNavigationDrawer(
        drawerContent = {
            PermanentDrawerSheet(
                drawerContainerColor = MaterialTheme.colorScheme.surfaceContainer
            ) {
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 28.dp)
                ) {
                    Spacer(modifier = Modifier.height(28.dp))
                    Text(
                        text = stringResource(io.upix.R.string.app_name),
                        style = MaterialTheme.typography.titleSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                    Spacer(modifier = Modifier.height(Spacing.lg))
                    items.forEach { screen ->
                        val selected = isScreenSelected(currentDestination, screen)
                        val title = stringResource(screen.titleRes)
                        NavigationDrawerItem(
                            icon = {
                                Icon(
                                    imageVector = screen.iconFor(selected),
                                    contentDescription = null
                                )
                            },
                            label = { Text(title) },
                            selected = selected,
                            onClick = { navigateToTab(navController, screen) },
                            modifier = Modifier.padding(vertical = 2.dp)
                        )
                    }
                }
            }
        }
    ) {
        content()
    }
}
