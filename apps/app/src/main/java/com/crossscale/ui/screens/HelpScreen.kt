package com.crossscale.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import com.crossscale.R
import com.crossscale.ui.theme.Spacing

@Composable
fun HelpScreen() {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = Spacing.xl)
            .padding(top = Spacing.md, bottom = Spacing.xxl),
        verticalArrangement = Arrangement.spacedBy(Spacing.md)
    ) {
        Text(
            text = stringResource(R.string.common_help),
            style = MaterialTheme.typography.titleLarge
        )

        val steps = listOf(
            R.string.help_step_1,
            R.string.help_step_2,
            R.string.help_step_3
        )
        steps.forEachIndexed { index, stepRes ->
            Text(
                text = "${index + 1}. ${stringResource(stepRes)}",
                style = MaterialTheme.typography.bodyMedium
            )
        }
    }
}
