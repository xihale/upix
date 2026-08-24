package com.crossscale.ui.screens

import android.content.Intent
import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.HelpOutline
import androidx.compose.material.icons.automirrored.outlined.ReceiptLong
import androidx.compose.material.icons.outlined.ChevronRight
import androidx.compose.material.icons.outlined.Info
import androidx.compose.material.icons.outlined.Link
import androidx.compose.material.icons.outlined.Lock
import androidx.compose.material.icons.outlined.Memory
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.adaptive.ExperimentalMaterial3AdaptiveApi
import androidx.compose.material3.adaptive.currentWindowAdaptiveInfo
import androidx.compose.material3.adaptive.layout.ListDetailPaneScaffoldRole
import androidx.compose.material3.adaptive.navigation.NavigableListDetailPaneScaffold
import androidx.compose.material3.adaptive.navigation.rememberListDetailPaneScaffoldNavigator
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.crossscale.R
import com.crossscale.ui.components.CrossScaleTopAppBar
import com.crossscale.ui.components.SectionHeader
import com.crossscale.ui.theme.EnterMotionSpec
import com.crossscale.ui.theme.LocalScreenMargin
import com.crossscale.ui.theme.MaxContentWidth
import com.crossscale.ui.theme.Spacing
import kotlinx.coroutines.launch

/** 设置详情子页：medium+ 双栏时的 detail pane 内容键。 */
enum class SettingsDetail { MODELS, LOGS, HELP }

/** MD3 medium 断点（600dp，整数 dp）。 */
private const val MEDIUM_BREAKPOINT_DP = 600

/**
 * 设置屏：分组卡列表（组间留白 Spacing.lg，组内行间留白，不用 Divider）。
 * compact 单栏（导航跳转子页）；medium+ 转 MD3 list-detail 双栏，
 * 子页内容经 [detailPaneContent] 注入 detail pane。
 */
@OptIn(ExperimentalMaterial3AdaptiveApi::class)
@Composable
fun SettingsScreen(
    selectedModelName: String?,
    onOpenModels: () -> Unit,
    onOpenLogs: () -> Unit,
    onOpenHelp: () -> Unit,
    isStreamEncrypted: Boolean,
    onStreamEncryptedChange: (Boolean) -> Unit,
    detailPaneContent: (@Composable (SettingsDetail) -> Unit)? = null
) {
    val useListDetail = detailPaneContent != null &&
        currentWindowAdaptiveInfo().windowSizeClass.isWidthAtLeastBreakpoint(MEDIUM_BREAKPOINT_DP)

    Scaffold(
        topBar = {
            CrossScaleTopAppBar(title = stringResource(R.string.common_settings))
        },
        containerColor = MaterialTheme.colorScheme.background,
        // 顶层 app shell 已统一消费系统栏 insets，这里不再叠加。
        contentWindowInsets = WindowInsets(0, 0, 0, 0)
    ) { innerPadding ->
        if (useListDetail) {
            SettingsListDetailPane(
                detailPaneContent = detailPaneContent,
                modifier = Modifier
                    .fillMaxSize()
                    .padding(innerPadding)
            ) { openDetail ->
                SettingsListBody(
                    selectedModelName = selectedModelName,
                    isStreamEncrypted = isStreamEncrypted,
                    onStreamEncryptedChange = onStreamEncryptedChange,
                    onOpenModels = { openDetail(SettingsDetail.MODELS) },
                    onOpenLogs = { openDetail(SettingsDetail.LOGS) },
                    onOpenHelp = { openDetail(SettingsDetail.HELP) }
                )
            }
        } else {
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(innerPadding),
                contentAlignment = Alignment.TopCenter
            ) {
                Column(
                    modifier = Modifier
                        .fillMaxSize()
                        .widthIn(max = MaxContentWidth)
                        .verticalScroll(rememberScrollState())
                        .padding(horizontal = LocalScreenMargin.current)
                        .padding(top = Spacing.md, bottom = Spacing.xxl),
                    verticalArrangement = Arrangement.spacedBy(Spacing.sm)
                ) {
                    SettingsListBody(
                        selectedModelName = selectedModelName,
                        isStreamEncrypted = isStreamEncrypted,
                        onStreamEncryptedChange = onStreamEncryptedChange,
                        onOpenModels = onOpenModels,
                        onOpenLogs = onOpenLogs,
                        onOpenHelp = onOpenHelp
                    )
                }
            }
        }
    }
}

/** medium+ 双栏：list pane 复用设置列表，detail pane 承载子页内容。 */
@OptIn(ExperimentalMaterial3AdaptiveApi::class)
@Composable
private fun SettingsListDetailPane(
    detailPaneContent: @Composable (SettingsDetail) -> Unit,
    modifier: Modifier = Modifier,
    listContent: @Composable ((SettingsDetail) -> Unit) -> Unit
) {
    val navigator = rememberListDetailPaneScaffoldNavigator<SettingsDetail>()
    val scope = rememberCoroutineScope()
    if (navigator.canNavigateBack()) {
        BackHandler { scope.launch { navigator.navigateBack() } }
    }

    NavigableListDetailPaneScaffold(
        navigator = navigator,
        modifier = modifier,
        listPane = {
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .verticalScroll(rememberScrollState())
                    .padding(horizontal = LocalScreenMargin.current)
                    .padding(top = Spacing.md, bottom = Spacing.xxl),
                verticalArrangement = Arrangement.spacedBy(Spacing.sm)
            ) {
                listContent { detail ->
                    scope.launch {
                        navigator.navigateTo(ListDetailPaneScaffoldRole.Detail, detail)
                    }
                }
            }
        },
        detailPane = {
            AnimatedContent(
                targetState = navigator.currentDestination?.contentKey,
                transitionSpec = {
                    fadeIn(EnterMotionSpec) togetherWith fadeOut(EnterMotionSpec)
                },
                label = "settingsDetailFade"
            ) { detail ->
                if (detail != null) {
                    detailPaneContent(detail)
                } else {
                    Box(
                        modifier = Modifier.fillMaxSize(),
                        contentAlignment = Alignment.Center
                    ) {
                        Text(
                            text = stringResource(R.string.settings_detail_empty),
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
            }
        }
    )
}

/** 设置列表主体：compact 单栏与 medium+ list pane 共用。 */
@Composable
private fun SettingsListBody(
    selectedModelName: String?,
    isStreamEncrypted: Boolean,
    onStreamEncryptedChange: (Boolean) -> Unit,
    onOpenModels: () -> Unit,
    onOpenLogs: () -> Unit,
    onOpenHelp: () -> Unit
) {
    Text(
        text = stringResource(
            R.string.settings_current_model,
            selectedModelName ?: stringResource(R.string.common_not_selected)
        ),
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant
    )

    SectionHeader(text = stringResource(R.string.settings_section_general))
    SettingsGroupCard {
        SettingsEntryRow(
            icon = Icons.Outlined.Memory,
            title = stringResource(R.string.common_model_management),
            onClick = onOpenModels
        )
        SettingsEntryRow(
            icon = Icons.AutoMirrored.Outlined.ReceiptLong,
            title = stringResource(R.string.common_logs),
            onClick = onOpenLogs
        )
        SettingsEntryRow(
            icon = Icons.AutoMirrored.Outlined.HelpOutline,
            title = stringResource(R.string.common_help),
            onClick = onOpenHelp
        )
    }

    SectionHeader(text = stringResource(R.string.settings_section_transfer))
    SettingsGroupCard {
        // 加密数据流传输开关: 控制图像/结果数据流是否加密(吞吐优先时关闭)。
        // 控制帧始终加密; 仅大 payload 走 stream 路径。
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .heightIn(min = 48.dp)
                .padding(horizontal = Spacing.md, vertical = Spacing.sm),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Icon(
                imageVector = Icons.Outlined.Lock,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.size(20.dp)
            )
            Column(
                modifier = Modifier
                    .weight(1f)
                    .padding(start = Spacing.md)
            ) {
                Text(
                    text = stringResource(R.string.settings_stream_encryption_title),
                    style = MaterialTheme.typography.bodyLarge
                )
                Text(
                    text = stringResource(R.string.settings_stream_encryption_subtitle),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            Switch(
                checked = isStreamEncrypted,
                onCheckedChange = onStreamEncryptedChange
            )
        }
    }

    SectionHeader(text = stringResource(R.string.settings_section_about))
    SettingsGroupCard {
        val context = LocalContext.current
        val versionName = remember {
            runCatching {
                context.packageManager.getPackageInfo(context.packageName, 0).versionName
            }.getOrNull()
        }
        SettingsEntryRow(
            icon = Icons.Outlined.Info,
            title = stringResource(R.string.settings_about_version, versionName ?: "-")
        )
        SettingsEntryRow(
            icon = Icons.Outlined.Link,
            title = stringResource(R.string.settings_about_project),
            subtitle = PROJECT_URL.removePrefix("https://"),
            onClick = {
                runCatching {
                    context.startActivity(
                        Intent(Intent.ACTION_VIEW, android.net.Uri.parse(PROJECT_URL))
                    )
                }
            }
        )
    }
}

/** 开源项目地址（关于页展示并可跳转）。 */
private const val PROJECT_URL = "https://github.com/xihale/crossscale"

@Composable
private fun SettingsGroupCard(content: @Composable () -> Unit) {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.large,
        color = MaterialTheme.colorScheme.surfaceContainer
    ) {
        Column(
            modifier = Modifier.padding(vertical = Spacing.sm),
            verticalArrangement = Arrangement.spacedBy(Spacing.xs)
        ) {
            content()
        }
    }
}

@Composable
private fun SettingsEntryRow(
    icon: ImageVector,
    title: String,
    subtitle: String? = null,
    onClick: (() -> Unit)? = null
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = 48.dp)
            .then(
                if (onClick != null) {
                    Modifier.clickable(onClick = onClick)
                } else {
                    Modifier
                }
            )
            .padding(horizontal = Spacing.md, vertical = Spacing.sm),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Icon(
            imageVector = icon,
            contentDescription = null,
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.size(20.dp)
        )
        Column(
            modifier = Modifier
                .weight(1f)
                .padding(start = Spacing.md)
        ) {
            Text(
                text = title,
                style = MaterialTheme.typography.bodyLarge
            )
            if (subtitle != null) {
                Text(
                    text = subtitle,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
        if (onClick != null) {
            Icon(
                imageVector = Icons.Outlined.ChevronRight,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.size(20.dp)
            )
        }
    }
}
