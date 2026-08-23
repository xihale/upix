package com.crossscale

import androidx.compose.foundation.layout.padding
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Scaffold
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.navigation.compose.rememberNavController
import com.crossscale.ui.components.BottomNavigationBar
import com.crossscale.ui.navigation.CrossScaleNavigation
import com.crossscale.ui.scanner.ScannerMode

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun CrossScaleApp(container: AppContainer) {
    val navController = rememberNavController()

    Scaffold(
        bottomBar = {
            // 扫码模式为全屏相机体验，底部导航栏直接隐藏（无过渡动画）。
            if (!ScannerMode.isActive) {
                BottomNavigationBar(navController = navController)
            }
        }
    ) { innerPadding ->
        CrossScaleNavigation(
            container = container,
            navController = navController,
            modifier = Modifier.padding(innerPadding)
        )
    }
}
