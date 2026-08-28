package io.upix.ui.screens

import android.annotation.SuppressLint
import android.content.Context
import android.content.Intent
import android.os.PowerManager
import android.provider.Settings
import androidx.activity.compose.BackHandler
import androidx.annotation.StringRes
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.outlined.HelpOutline
import androidx.compose.material.icons.automirrored.outlined.ReceiptLong
import androidx.compose.material.icons.outlined.BatterySaver
import androidx.compose.material.icons.outlined.ChevronRight
import androidx.compose.material.icons.outlined.Info
import androidx.compose.material.icons.outlined.Link
import androidx.compose.material.icons.outlined.Lock
import androidx.compose.material.icons.outlined.Memory
import androidx.compose.material.icons.outlined.Tune
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.adaptive.ExperimentalMaterial3AdaptiveApi
import androidx.compose.material3.adaptive.currentWindowAdaptiveInfo
import androidx.compose.material3.adaptive.layout.ListDetailPaneScaffoldRole
import androidx.compose.material3.adaptive.navigation.ThreePaneScaffoldNavigator
import androidx.compose.material3.adaptive.navigation.rememberListDetailPaneScaffoldNavigator
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import io.upix.R
import io.upix.data.repository.EncoderSettings
import io.upix.ui.components.UpixTopAppBar
import io.upix.ui.components.SectionHeader
import io.upix.ui.theme.EnterMotionSpec
import io.upix.ui.theme.LocalScreenMargin
import io.upix.ui.theme.MaxContentWidth
import io.upix.ui.theme.Spacing
import io.upix.ui.theme.pressable
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlin.math.roundToInt
import androidx.core.net.toUri

/** 设置详情子页：medium+ 双栏时的 detail pane 内容键 + 子页标题资源。 */
enum class SettingsDetail(@StringRes val titleRes: Int) {
    MODELS(R.string.common_model_management),
    LOGS(R.string.common_logs),
    HELP(R.string.common_help),
    ENCODER(R.string.common_encoder_settings)
}

/** MD3 断点（dp，整数）：medium=600，expanded=840（与 app shell 一致）。 */
private const val MEDIUM_BREAKPOINT_DP = 600
private const val EXPANDED_BREAKPOINT_DP = 840

/**
 * 设置屏：分区标题（[SectionHeader]）+ 分组卡，节间大留白、卡内统一行节奏，
 * 不用 Divider。compact 单栏（导航跳转子页）；medium+ 转 MD3 list-detail 双栏，
 * 子页内容经 [detailPaneContent] 注入 detail pane。
 */
@OptIn(ExperimentalMaterial3AdaptiveApi::class)
@Composable
fun SettingsScreen(
    onOpenModels: () -> Unit,
    onOpenLogs: () -> Unit,
    onOpenHelp: () -> Unit,
    onOpenEncoder: () -> Unit = {},
    detailPaneContent: (@Composable (SettingsDetail) -> Unit)? = null
) {
    val windowSizeClass = currentWindowAdaptiveInfo().windowSizeClass
    val useListDetail = detailPaneContent != null &&
        windowSizeClass.isWidthAtLeastBreakpoint(MEDIUM_BREAKPOINT_DP)

    if (useListDetail) {
        // medium(600-839dp) 单 pane 阶段点入子页后 list 被收起、detail 全屏，
        // 顶栏随之切为子页标题 + 返回箭头（对齐 compact 路径的二级屏顶栏）；
        // expanded(840dp+) 双栏 list 常驻，顶栏保持「设置」、detail pane 自带节标题。
        val navigator = rememberListDetailPaneScaffoldNavigator<SettingsDetail>()
        val scope = rememberCoroutineScope()
        if (navigator.canNavigateBack()) {
            BackHandler { scope.launch { navigator.navigateBack() } }
        }
        val detailInTopBar = navigator.currentDestination?.contentKey?.takeIf {
            !windowSizeClass.isWidthAtLeastBreakpoint(EXPANDED_BREAKPOINT_DP)
        }
        Scaffold(
            topBar = {
                UpixTopAppBar(
                    title = stringResource(detailInTopBar?.titleRes ?: R.string.common_settings),
                    navigationIcon = if (detailInTopBar != null) {
                        {
                            IconButton(onClick = { scope.launch { navigator.navigateBack() } }) {
                                Icon(
                                    imageVector = Icons.AutoMirrored.Filled.ArrowBack,
                                    contentDescription = stringResource(R.string.cd_back)
                                )
                            }
                        }
                    } else {
                        { }
                    }
                )
            },
            containerColor = MaterialTheme.colorScheme.background,
            // 顶层 app shell 已统一消费系统栏 insets，这里不再叠加。
            contentWindowInsets = WindowInsets(0, 0, 0, 0)
        ) { innerPadding ->
            SettingsListDetailPane(
                navigator = navigator,
                showDetailHeader = windowSizeClass.isWidthAtLeastBreakpoint(EXPANDED_BREAKPOINT_DP),
                detailPaneContent = detailPaneContent,
                modifier = Modifier
                    .fillMaxSize()
                    .padding(innerPadding)
            ) { openDetail ->
                SettingsListBody(
                    onOpenModels = { openDetail(SettingsDetail.MODELS) },
                    onOpenLogs = { openDetail(SettingsDetail.LOGS) },
                    onOpenHelp = { openDetail(SettingsDetail.HELP) },
                    onOpenEncoder = { openDetail(SettingsDetail.ENCODER) }
                )
            }
        }
    } else {
        Scaffold(
            topBar = {
                UpixTopAppBar(title = stringResource(R.string.common_settings))
            },
            containerColor = MaterialTheme.colorScheme.background,
            // 顶层 app shell 已统一消费系统栏 insets，这里不再叠加。
            contentWindowInsets = WindowInsets(0, 0, 0, 0)
        ) { innerPadding ->
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
                    verticalArrangement = Arrangement.spacedBy(Spacing.md)
                ) {
                    SettingsListBody(
                        onOpenModels = onOpenModels,
                        onOpenLogs = onOpenLogs,
                        onOpenHelp = onOpenHelp,
                        onOpenEncoder = onOpenEncoder
                    )
                }
            }
        }
    }
}

/** 设置详情空态：expanded 下未选子页时 detail pane 的占位。 */
@Composable
private fun SettingsDetailEmpty(modifier: Modifier = Modifier) {
    Box(
        modifier = modifier.fillMaxSize(),
        contentAlignment = Alignment.Center
    ) {
        Text(
            text = stringResource(R.string.settings_detail_empty),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }
}

/**
 * medium+ 设置双栏。不再用 NavigableListDetailPaneScaffold 渲染：
 * 其内置 pane 转场是边界膨胀动效（detail 从列表位置"弹出"），与设计语言 §7 的
 * 安静 fade 冲突且无公开参数可关。这里保留 navigator 管理返回栈与返回手势，
 * 仅自行排布两个 pane，切换统一走 [EnterMotionSpec] 纯 fade。
 */
@OptIn(ExperimentalMaterial3AdaptiveApi::class)
@Composable
private fun SettingsListDetailPane(
    navigator: ThreePaneScaffoldNavigator<SettingsDetail>,
    showDetailHeader: Boolean,
    detailPaneContent: @Composable (SettingsDetail) -> Unit,
    modifier: Modifier = Modifier,
    listContent: @Composable ((SettingsDetail) -> Unit) -> Unit
) {
    val scope = rememberCoroutineScope()
    val openDetail: (SettingsDetail) -> Unit = { detail ->
        scope.launch { navigator.navigateTo(ListDetailPaneScaffoldRole.Detail, detail) }
    }
    val currentDetail = navigator.currentDestination?.contentKey

    if (showDetailHeader) {
        // expanded：list 常驻左栏（1/3），detail 右栏随选中项 fade 切换。
        Row(modifier = modifier.fillMaxSize()) {
            Column(
                modifier = Modifier
                    .weight(1f)
                    .fillMaxHeight()
                    .verticalScroll(rememberScrollState())
                    .padding(horizontal = LocalScreenMargin.current)
                    .padding(top = Spacing.md, bottom = Spacing.xxl),
                verticalArrangement = Arrangement.spacedBy(Spacing.md)
            ) {
                listContent(openDetail)
            }
            Box(modifier = Modifier.weight(2f).fillMaxHeight()) {
                AnimatedContent(
                    targetState = currentDetail,
                    transitionSpec = {
                        fadeIn(EnterMotionSpec) togetherWith fadeOut(EnterMotionSpec)
                    },
                    label = "settingsDetailFade"
                ) { detail ->
                    if (detail == null) {
                        SettingsDetailEmpty()
                    } else {
                        Column(modifier = Modifier.fillMaxSize()) {
                            SectionHeader(
                                text = stringResource(detail.titleRes),
                                modifier = Modifier.padding(
                                    start = LocalScreenMargin.current,
                                    top = Spacing.md,
                                    end = LocalScreenMargin.current
                                )
                            )
                            Box(
                                modifier = Modifier
                                    .weight(1f)
                                    .fillMaxWidth()
                            ) {
                                detailPaneContent(detail)
                            }
                        }
                    }
                }
            }
        }
    } else {
        // medium 单 pane 阶段：list/detail 整页互切，同样纯 fade。
        // 注意把外层 modifier（含 Scaffold 的 innerPadding）传进来，
        // 否则列表会顶到状态栏/顶栏底下（遮挡首项）。
        AnimatedContent(
            targetState = currentDetail,
            transitionSpec = {
                fadeIn(EnterMotionSpec) togetherWith fadeOut(EnterMotionSpec)
            },
            modifier = modifier,
            label = "settingsPageFade"
        ) { detail ->
            if (detail == null) {
                Column(
                    modifier = Modifier
                        .fillMaxSize()
                        .verticalScroll(rememberScrollState())
                        .padding(horizontal = LocalScreenMargin.current)
                        .padding(top = Spacing.md, bottom = Spacing.xxl),
                    verticalArrangement = Arrangement.spacedBy(Spacing.md)
                ) {
                    listContent(openDetail)
                }
            } else {
                detailPaneContent(detail)
            }
        }
    }
}

/** 设置列表主体：compact 单栏与 medium+ list pane 共用。节间留白 > 卡内行距。 */
@Composable
private fun SettingsListBody(
    onOpenModels: () -> Unit,
    onOpenLogs: () -> Unit,
    onOpenHelp: () -> Unit,
    onOpenEncoder: () -> Unit
) {
    SettingsSection(title = stringResource(R.string.settings_section_general)) {
        SettingsEntryRow(
            icon = Icons.Outlined.Memory,
            title = stringResource(R.string.common_model_management),
            onClick = onOpenModels
        )
        SettingsEntryRow(
            icon = Icons.Outlined.Tune,
            title = stringResource(R.string.common_encoder_settings),
            onClick = onOpenEncoder
        )
        // 电池策略入口：远程批处理靠后台 Wi-Fi 收发，屏灭后厂商电源管理
        // （HarmonyOS/MIUI）会掐断传输，须在系统设置里设为不受限。
        // 已不受限时整行不显示；状态每次回到前台重读，从系统设置页返回即生效。
        val context = LocalContext.current
        var batteryUnrestricted by remember { mutableStateOf(isBatteryUnrestricted(context)) }
        val lifecycleOwner = LocalLifecycleOwner.current
        DisposableEffect(lifecycleOwner) {
            val observer = LifecycleEventObserver { _, event ->
                if (event == Lifecycle.Event.ON_RESUME) {
                    batteryUnrestricted = isBatteryUnrestricted(context)
                }
            }
            lifecycleOwner.lifecycle.addObserver(observer)
            onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
        }
        if (!batteryUnrestricted) {
            SettingsEntryRow(
                icon = Icons.Outlined.BatterySaver,
                title = stringResource(R.string.settings_battery_policy),
                onClick = { openBatteryPolicySettings(context) }
            )
        }
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

    SettingsSection(title = stringResource(R.string.settings_section_about)) {
        val context = LocalContext.current
        // getPackageInfo 是 PackageManager binder IPC：异步在 IO 线程读取，
        // 组合期不再阻塞主线程；加载中先以「-」占位。
        val versionName by produceState<String?>(initialValue = null, context) {
            value = withContext(Dispatchers.IO) {
                runCatching {
                    context.packageManager.getPackageInfo(context.packageName, 0).versionName
                }.getOrNull()
            }
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
                        Intent(Intent.ACTION_VIEW, PROJECT_URL.toUri())
                    )
                }
            }
        )
    }
}

/** 开源项目地址（关于页展示并可跳转）。 */
private const val PROJECT_URL = "https://github.com/xihale/upix"

/** 当前应用是否已加入电池优化白名单（不受限）。 */
private fun isBatteryUnrestricted(context: Context): Boolean {
    val powerManager = context.getSystemService(Context.POWER_SERVICE) as? PowerManager
        ?: return false
    return powerManager.isIgnoringBatteryOptimizations(context.packageName)
}

/**
 * 跳转本应用电池策略设置：优先拉起单应用「忽略电池优化」确认框（AOSP
 * 直达不受限开关；厂商 ROM 多路由到自家启动/省电管理页），不可用再退
 * 全局电池优化列表、应用详情页。逐个尝试，首个能拉起的生效。
 *
 * BatteryLife 为 Play 上架政策提示：本应用 GitHub 分发，此入口即保活
 * （防屏灭掉线）方案的组成部分，属预期用法。
 */
@SuppressLint("BatteryLife")
private fun openBatteryPolicySettings(context: Context) {
    val packageUri = "package:${context.packageName}".toUri()
    listOf(
        Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, packageUri),
        Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS),
        Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, packageUri)
    ).forEach { intent ->
        if (runCatching { context.startActivity(intent) }.isSuccess) {
            return
        }
    }
}

/** 一个设置分区：小节标题 + 紧随其后的分组卡。 */
@Composable
private fun SettingsSection(
    title: String,
    content: @Composable () -> Unit
) {
    Column(verticalArrangement = Arrangement.spacedBy(Spacing.sm)) {
        SectionHeader(text = title)
        SettingsGroupCard(content = content)
    }
}

@Composable
private fun SettingsGroupCard(content: @Composable () -> Unit) {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.medium,
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

/**
 * 设置行统一骨架：图标 + 标题/副标题 + 尾部插槽；有 [onClick] 时整行可点
 * （涟漪 + 按压缩放），尾部自动补 chevron。56dp 最小高度保证舒适触达。
 */
@Composable
private fun SettingsRow(
    icon: ImageVector,
    title: String,
    subtitle: String? = null,
    onClick: (() -> Unit)? = null,
    trailing: (@Composable () -> Unit)? = null
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = 56.dp)
            .then(
                if (onClick != null) {
                    Modifier.pressable(onClick = onClick)
                } else {
                    Modifier
                }
            )
            .padding(horizontal = Spacing.lg, vertical = Spacing.sm),
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
                .padding(horizontal = Spacing.lg)
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
        if (trailing != null) {
            trailing()
        } else if (onClick != null) {
            Icon(
                imageVector = Icons.Outlined.ChevronRight,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.size(20.dp)
            )
        }
    }
}

/** 可点击的入口行（默认尾部 chevron）；无 [onClick] 时为静态信息行。 */
@Composable
private fun SettingsEntryRow(
    icon: ImageVector,
    title: String,
    subtitle: String? = null,
    onClick: (() -> Unit)? = null
) {
    SettingsRow(icon = icon, title = title, subtitle = subtitle, onClick = onClick)
}

// ── 编码高级设置子页 ─────────────────────────────────────────────────────────

/**
 * 编码高级设置：全局质量 + 各格式独立旋钮，分组卡承载。`showAvif` 为本地
 * 编码能力或远程可用（AVIF 组的参数对 PC 远程编码同样生效）。
 */
@Composable
fun EncoderSettingsPane(
    settings: EncoderSettings,
    showAvif: Boolean,
    onChange: (EncoderSettings) -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .widthIn(max = MaxContentWidth)
            .verticalScroll(rememberScrollState())
            .padding(horizontal = LocalScreenMargin.current)
            .padding(top = Spacing.md, bottom = Spacing.xxl),
        verticalArrangement = Arrangement.spacedBy(Spacing.md)
    ) {
        SettingsSection(title = stringResource(R.string.settings_encoder_quality)) {
            SettingsSliderRow(
                title = stringResource(R.string.settings_encoder_quality),
                hint = stringResource(R.string.settings_encoder_quality_hint),
                valueLabel = settings.normalizedQuality.toString(),
                value = settings.normalizedQuality.toFloat(),
                valueRange = 1f..100f
            ) { value -> onChange(settings.copy(quality = value)) }
        }

        SettingsSection(title = "PNG") {
            SettingsSwitchRow(
                title = stringResource(R.string.settings_encoder_png_level),
                subtitle = stringResource(R.string.settings_encoder_png_level_hint),
                checked = settings.pngLevel >= 0,
                onCheckedChange = { custom ->
                    onChange(settings.copy(pngLevel = if (custom) 6 else -1))
                }
            )
            if (settings.pngLevel >= 0) {
                SettingsSliderRow(
                    title = stringResource(R.string.settings_encoder_png_level),
                    valueLabel = settings.pngLevel.toString(),
                    value = settings.pngLevel.toFloat(),
                    valueRange = 0f..9f,
                    steps = 8
                ) { value -> onChange(settings.copy(pngLevel = value)) }
            }
        }

        SettingsSection(title = "JPEG") {
            SettingsSegmentedRow(
                title = stringResource(R.string.settings_encoder_jpeg_subsampling),
                options = listOf(420, 422, 444),
                selected = settings.jpegSubsampling,
                onSelect = { value -> onChange(settings.copy(jpegSubsampling = value)) }
            )
        }

        SettingsSection(title = "WebP") {
            SettingsSwitchRow(
                title = stringResource(R.string.settings_encoder_webp_method),
                subtitle = stringResource(R.string.settings_encoder_webp_method_hint),
                checked = settings.webpMethod >= 0,
                onCheckedChange = { custom ->
                    onChange(settings.copy(webpMethod = if (custom) 4 else -1))
                }
            )
            if (settings.webpMethod >= 0) {
                SettingsSliderRow(
                    title = stringResource(R.string.settings_encoder_webp_method),
                    valueLabel = settings.webpMethod.toString(),
                    value = settings.webpMethod.toFloat(),
                    valueRange = 0f..6f,
                    steps = 5
                ) { value -> onChange(settings.copy(webpMethod = value)) }
            }
            SettingsSwitchRow(
                title = stringResource(R.string.settings_encoder_webp_lossless),
                checked = settings.webpLossless,
                onCheckedChange = { value -> onChange(settings.copy(webpLossless = value)) }
            )
        }

        if (showAvif) {
            SettingsSection(title = stringResource(R.string.settings_encoder_avif_group)) {
                SettingsSliderRow(
                    title = stringResource(R.string.settings_encoder_avif_speed),
                    hint = stringResource(R.string.settings_encoder_avif_speed_hint),
                    valueLabel = settings.avifSpeed.toString(),
                    value = settings.avifSpeed.toFloat(),
                    valueRange = 0f..10f,
                    steps = 9
                ) { value -> onChange(settings.copy(avifSpeed = value)) }
                SettingsSegmentedRow(
                    title = stringResource(R.string.settings_encoder_avif_subsampling),
                    options = listOf(420, 444),
                    selected = settings.avifSubsampling,
                    onSelect = { value -> onChange(settings.copy(avifSubsampling = value)) }
                )
            }
        }
    }
}

/** 标题 + 提示 + 当前值 + 滑杆的设置行。onValue 回传取整后的值。 */
@Composable
private fun SettingsSliderRow(
    title: String,
    hint: String? = null,
    valueLabel: String,
    value: Float,
    valueRange: ClosedFloatingPointRange<Float>,
    steps: Int = 0,
    onValue: (Int) -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = Spacing.lg, vertical = Spacing.sm)
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(modifier = Modifier.weight(1f)) {
                Text(text = title, style = MaterialTheme.typography.bodyLarge)
                if (hint != null) {
                    Text(
                        text = hint,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
            Text(
                text = valueLabel,
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
        Slider(
            value = value,
            onValueChange = { raw -> onValue(raw.roundToInt()) },
            valueRange = valueRange,
            steps = steps,
            modifier = Modifier.fillMaxWidth()
        )
    }
}

/** 标题 +（可选）提示 + 尾部开关的设置行。 */
@Composable
private fun SettingsSwitchRow(
    title: String,
    subtitle: String? = null,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = 56.dp)
            .padding(horizontal = Spacing.lg, vertical = Spacing.sm),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(text = title, style = MaterialTheme.typography.bodyLarge)
            if (subtitle != null) {
                Text(
                    text = subtitle,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
        Switch(checked = checked, onCheckedChange = onCheckedChange)
    }
}

/** 标题 + 分段单选的设置行。 */
@Composable
private fun SettingsSegmentedRow(
    title: String,
    options: List<Int>,
    selected: Int,
    onSelect: (Int) -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = Spacing.lg, vertical = Spacing.sm)
    ) {
        Text(text = title, style = MaterialTheme.typography.bodyLarge)
        SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth().padding(top = Spacing.xs)) {
            options.forEachIndexed { index, option ->
                SegmentedButton(
                    selected = option == selected,
                    onClick = { onSelect(option) },
                    shape = SegmentedButtonDefaults.itemShape(index = index, count = options.size)
                ) {
                    Text(text = option.toString())
                }
            }
        }
    }
}

