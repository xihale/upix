package com.crossscale

import androidx.camera.core.ExperimentalGetImage
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Scaffold
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.navigation.compose.rememberNavController
import com.crossscale.ui.components.BottomNavigationBar
import com.crossscale.ui.navigation.CrossScaleNavigation

@OptIn(ExperimentalMaterial3Api::class)
@ExperimentalGetImage
@Composable
fun CrossScaleApp(container: AppContainer) {
    val navController = rememberNavController()

    Scaffold(
        bottomBar = { BottomNavigationBar(navController = navController) }
    ) { innerPadding ->
        CrossScaleNavigation(
            container = container,
            navController = navController,
            modifier = Modifier.padding(innerPadding)
        )
    }
}
