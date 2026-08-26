package io.upix.ui.components

import androidx.compose.material3.NavigationBar
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.res.stringResource
import androidx.navigation.NavController
import io.upix.ui.navigation.AppScreen

/**
 * compact（<600dp）底部导航栏。
 * 选中态判定、图标选择与目的地切换策略与 rail/drawer 共享（见 AdaptiveNavigation）。
 */
@Composable
fun BottomNavigationBar(navController: NavController) {
    NavigationBar(containerColor = MaterialTheme.colorScheme.surfaceContainer) {
        val currentDestination = currentDestinationOf(navController)

        AppScreen.bottomNavigationItems.forEach { screen ->
            val selected = isScreenSelected(currentDestination, screen)
            val title = stringResource(screen.titleRes)

            NavigationBarItem(
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
