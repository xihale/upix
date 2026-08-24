package com.crossscale

import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Scaffold
import androidx.compose.material3.adaptive.ExperimentalMaterial3AdaptiveApi
import androidx.compose.material3.adaptive.currentWindowAdaptiveInfo
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.Modifier
import androidx.navigation.compose.rememberNavController
import com.crossscale.ui.components.AdaptiveNavigationRail
import com.crossscale.ui.components.AppNavigationDrawer
import com.crossscale.ui.components.BottomNavigationBar
import com.crossscale.ui.navigation.CrossScaleNavigation
import com.crossscale.ui.scanner.ScannerMode
import com.crossscale.ui.theme.LocalScreenMargin
import com.crossscale.ui.theme.Spacing

/**
 * MD3 adaptive app shell：按窗口宽度类切换导航形态——
 * compact(<600dp) NavigationBar / medium(600-839dp) NavigationRail /
 * expanded+(840dp+) 常驻抽屉。页面边距 token（16/24dp）随档位经
 * LocalScreenMargin 下发。
 */
@OptIn(ExperimentalMaterial3Api::class, ExperimentalMaterial3AdaptiveApi::class)
@Composable
fun CrossScaleApp(container: AppContainer) {
    val navController = rememberNavController()
    val windowSizeClass = currentWindowAdaptiveInfo().windowSizeClass
    // isWidthAtLeastBreakpoint 参数为 dp 整数值（600 = medium、840 = expanded）。
    val isMedium = windowSizeClass.isWidthAtLeastBreakpoint(600)
    val isExpanded = windowSizeClass.isWidthAtLeastBreakpoint(840)
    val scannerActive = ScannerMode.isActive
    // 扫码页自管系统栏沉浸；rail/drawer 模式（无 bottomBar）由外层消费导航栏 insets。
    val navigationModifier = if (scannerActive) {
        Modifier
    } else {
        Modifier.navigationBarsPadding()
    }

    CompositionLocalProvider(
        LocalScreenMargin provides if (isMedium) Spacing.xxl else Spacing.lg
    ) {
        when {
            // compact：底部导航栏。
            !isMedium -> Scaffold(
                bottomBar = {
                    // 扫码模式为全屏相机体验，底部导航栏直接隐藏（无过渡动画）。
                    if (!scannerActive) {
                        BottomNavigationBar(navController = navController)
                    }
                },
                // 各屏自带 Scaffold + TopAppBar：statusBars 由顶栏消费，
                // 底部由 bottomBar(NavigationBar) 占位，这里不再叠加系统栏 insets。
                contentWindowInsets = WindowInsets(0, 0, 0, 0)
            ) { innerPadding ->
                CrossScaleNavigation(
                    container = container,
                    navController = navController,
                    modifier = Modifier.padding(innerPadding)
                )
            }

            // medium：侧边导航栏。
            !isExpanded -> Row(modifier = Modifier.fillMaxSize()) {
                if (!scannerActive) {
                    AdaptiveNavigationRail(navController = navController)
                }
                CrossScaleNavigation(
                    container = container,
                    navController = navController,
                    modifier = Modifier
                        .weight(1f)
                        .fillMaxHeight()
                        .then(navigationModifier)
                )
            }

            // expanded+：常驻侧边抽屉。
            else -> AppNavigationDrawer(navController = navController) {
                CrossScaleNavigation(
                    container = container,
                    navController = navController,
                    modifier = Modifier
                        .fillMaxSize()
                        .then(navigationModifier)
                )
            }
        }
    }
}
