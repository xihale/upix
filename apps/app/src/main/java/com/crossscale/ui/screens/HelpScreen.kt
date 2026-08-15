package com.crossscale.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
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

        FaqCard(
            question = stringResource(R.string.help_faq_q1),
            answer = stringResource(R.string.help_step_1),
        )
        FaqCard(
            question = stringResource(R.string.help_faq_q2),
            answer = stringResource(R.string.help_step_2),
        )
        FaqCard(
            question = stringResource(R.string.help_faq_q3),
            answer = stringResource(R.string.help_step_3),
        )
        FaqCard(
            question = stringResource(R.string.help_faq_q4),
            answer = stringResource(R.string.help_step_4),
        )
        FaqCard(
            question = stringResource(R.string.help_faq_q5),
            answer = stringResource(R.string.help_step_5),
        )

        Surface(
            modifier = Modifier.fillMaxWidth(),
            shape = MaterialTheme.shapes.medium,
            color = MaterialTheme.colorScheme.secondaryContainer,
            contentColor = MaterialTheme.colorScheme.onSecondaryContainer
        ) {
            Text(
                text = stringResource(R.string.help_tip),
                style = MaterialTheme.typography.bodyMedium,
                modifier = Modifier.padding(Spacing.lg)
            )
        }
    }
}

@Composable
private fun FaqCard(question: String, answer: String) {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.medium,
        color = MaterialTheme.colorScheme.surfaceContainer
    ) {
        Column(
            modifier = Modifier.padding(Spacing.lg),
            verticalArrangement = Arrangement.spacedBy(Spacing.sm)
        ) {
            Text(
                text = question,
                style = MaterialTheme.typography.titleSmall
            )
            Text(
                text = answer,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}
