package io.upix

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
import androidx.compose.runtime.movableContentOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.navigation.compose.rememberNavController
import io.upix.ui.components.AdaptiveNavigationRail
import io.upix.ui.components.AppNavigationDrawer
import io.upix.ui.components.BottomNavigationBar
import io.upix.ui.navigation.UpixNavigation
import io.upix.ui.scanner.ScannerMode
import io.upix.ui.theme.LocalScreenMargin
import io.upix.ui.theme.Spacing

/** MD3 adaptive 断点（dp）：medium=600、expanded=840。 */
private const val MediumWidthBreakpointDp = 600
private const val ExpandedWidthBreakpointDp = 840

/**
 * MD3 adaptive app shell：按窗口宽度类切换导航形态，
 * compact(<600dp) NavigationBar / medium(600-839dp) NavigationRail /
 * expanded+(840dp+) 常驻抽屉。页面边距 token（16/24dp）随档位经
 * LocalScreenMargin 下发。
 */
@OptIn(ExperimentalMaterial3Api::class, ExperimentalMaterial3AdaptiveApi::class)
@Composable
fun UpixApp(container: AppContainer) {
    val navController = rememberNavController()
    val windowSizeClass = currentWindowAdaptiveInfo().windowSizeClass
    // isWidthAtLeastBreakpoint 参数为 dp 整数值。
    val isMedium = windowSizeClass.isWidthAtLeastBreakpoint(MediumWidthBreakpointDp)
    val isExpanded = windowSizeClass.isWidthAtLeastBreakpoint(ExpandedWidthBreakpointDp)
    val scannerActive = ScannerMode.isActive
    // 扫码页自管系统栏沉浸；rail/drawer 模式（无 bottomBar）由外层消费导航栏 insets。
    val navigationModifier = if (scannerActive) {
        Modifier
    } else {
        Modifier.navigationBarsPadding()
    }

    // 导航子树必须以 movableContent 挂载：扫码开关会驱动下方 if 分支切换
    // app shell 结构，若直接在每个分支调用 UpixNavigation，NavHost 子树会在
    // 分支切换时被销毁重建——其中所有 rememberSaveable（包括扫码开关
    // showQrScanner 自身）随之丢失，扫码页进入组合后立即被打回（横竖屏
    // 点击扫码均秒退）。movable 让子树跨分支整体移动，状态与 effect 保留，
    // ScannerMode 也不会因销毁侧 onDispose->exit() 抖动。
    val appNav = remember {
        movableContentOf { modifier: Modifier ->
            UpixNavigation(container = container, navController = navController, modifier = modifier)
        }
    }

    CompositionLocalProvider(
        LocalScreenMargin provides if (isMedium) Spacing.xxl else Spacing.lg
    ) {
        if (scannerActive) {
            // 扫码全屏：compact/medium/expanded 一律不挂 bottomBar/rail/drawer，
            // 相机页自管系统栏沉浸。也不锁方向——平板上 requestOrientation 会
            // 触发 MIUI letterbox 反复重建。
            appNav(Modifier.fillMaxSize())
        } else when {
            // compact：底部导航栏。
            !isMedium -> Scaffold(
                bottomBar = {
                    BottomNavigationBar(navController = navController)
                },
                // 各屏自带 Scaffold + TopAppBar：statusBars 由顶栏消费，
                // 底部由 bottomBar(NavigationBar) 占位，这里不再叠加系统栏 insets。
                contentWindowInsets = WindowInsets(0, 0, 0, 0)
            ) { innerPadding ->
                appNav(Modifier.padding(innerPadding))
            }

            // medium：侧边导航栏。
            !isExpanded -> Row(modifier = Modifier.fillMaxSize()) {
                AdaptiveNavigationRail(navController = navController)
                appNav(
                    Modifier
                        .weight(1f)
                        .fillMaxHeight()
                        .then(navigationModifier)
                )
            }

            // expanded+：常驻侧边抽屉。
            else -> AppNavigationDrawer(navController = navController) {
                appNav(
                    Modifier
                        .fillMaxSize()
                        .then(navigationModifier)
                )
            }
        }
    }
}
