package com.crossscale

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.tooling.preview.Preview
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import com.crossscale.ui.screens.HomeScreen
import com.crossscale.ui.screens.DevicesScreen
import com.crossscale.ui.screens.ModelsScreen
import com.crossscale.ui.screens.LogsScreen
import com.crossscale.ui.theme.CrossScaleTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            CrossScaleTheme {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background
                ) {
                    CrossScaleApp()
                }
            }
        }
    }
}

@Composable
fun CrossScaleApp() {
    val navController = rememberNavController()
    
    Scaffold(
        bottomBar = { BottomNavigationBar(navController) }
    ) { innerPadding ->
        NavHost(
            navController = navController,
            startDestination = Screen.Home.route,
            modifier = Modifier.padding(innerPadding)
        ) {
            composable(Screen.Home.route) { HomeScreen() }
            composable(Screen.Devices.route) { DevicesScreen() }
            composable(Screen.Models.route) { ModelsScreen() }
            composable(Screen.Logs.route) { LogsScreen() }
        }
    }
}

sealed class Screen(val route: String, val title: String, val icon: Int) {
    object Home : Screen("home", "Home", android.R.drawable.ic_menu_home)
    object Devices : Screen("devices", "Devices", android.R.drawable.ic_menu_search)
    object Models : Screen("models", "Models", android.R.drawable.ic_menu_gallery)
    object Logs : Screen("logs", "Logs", android.R.drawable.ic_menu_info_details)
}

@Preview(showBackground = true)
@Composable
fun DefaultPreview() {
    CrossScaleTheme {
        CrossScaleApp()
    }
}
