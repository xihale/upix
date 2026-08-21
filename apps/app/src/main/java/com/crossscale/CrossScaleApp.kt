package com.crossscale

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.camera.core.ExperimentalGetImage
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
@ExperimentalGetImage
@Composable
fun CrossScaleApp(container: AppContainer) {
    val navController = rememberNavController()

    Scaffold(
        bottomBar = {
            // 扫码模式为全屏相机体验，底部导航栏滑出隐藏。
            AnimatedVisibility(
                visible = !ScannerMode.isActive,
                enter = fadeIn() + slideInVertically { it },
                exit = slideOutVertically { it } + fadeOut()
            ) {
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
