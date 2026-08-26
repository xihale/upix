package io.upix.ui.scanner

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.provider.Settings
import android.util.Log
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.camera.core.resolutionselector.ResolutionSelector
import androidx.camera.core.resolutionselector.ResolutionStrategy
import androidx.camera.core.CameraSelector
import androidx.camera.core.FocusMeteringAction
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import androidx.camera.core.Preview
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.view.PreviewView
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Close
import androidx.compose.material.icons.outlined.ErrorOutline
import androidx.compose.material.icons.outlined.PhotoCamera
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import io.upix.R
import io.upix.ui.theme.Spacing
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

private const val TAG = "UPIXScanner"
private const val REPORT_FRAMES = 30

@Composable
fun QRScannerScreen(
    onPayloadScanned: (String) -> Unit,
    onClose: () -> Unit
) {
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current
    val permissionDeniedMessage = stringResource(R.string.scanner_permission_denied)
    val scanFailedMessage = stringResource(R.string.scanner_scan_failed)
    val cameraFailedMessage = stringResource(R.string.scanner_camera_failed)
    val previewView = remember {
        PreviewView(context).apply {
            scaleType = PreviewView.ScaleType.FILL_CENTER
        }
    }
    var scanError by remember { mutableStateOf<String?>(null) }
    var cameraPermissionGranted by remember {
        mutableStateOf(
            ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) ==
                PackageManager.PERMISSION_GRANTED
        )
    }

    // 用户可能跳到系统设置改权限后返回（onResume 重新进前台）：ON_RESUME 时
    // 重查权限并更新快照，避免本页停留期间权限状态与 UI 脱节。
    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) {
                cameraPermissionGranted = ContextCompat.checkSelfPermission(
                    context,
                    Manifest.permission.CAMERA
                ) == PackageManager.PERMISSION_GRANTED
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }

    val cameraExecutor = remember { Executors.newSingleThreadExecutor() }
    val hasScanned = remember { AtomicBoolean(false) }
    // 相机分析线程上的回调统一切回主线程（状态写与外部回调都不在后台线程执行）。
    val mainExecutor = remember(context) { ContextCompat.getMainExecutor(context) }

    val requestPermissionLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.RequestPermission()
    ) { granted ->
        cameraPermissionGranted = granted
        if (!granted) {
            scanError = permissionDeniedMessage
        }
    }

    LaunchedEffect(Unit) {
        if (!cameraPermissionGranted) {
            requestPermissionLauncher.launch(Manifest.permission.CAMERA)
        }
    }

    DisposableEffect(Unit) {
        ScannerMode.enter()
        // 不在此处锁定方向：平板/MIUI 上 requestOrientation 会触发 letterbox
        // 信箱化并在横竖窗间反复重建；改为 ScannerMode 让宿主 shell 隐藏全部
        // 导航组件，扫码页在任意方向都是全屏。
        onDispose {
            ScannerMode.exit()
            cameraExecutor.shutdown()
        }
    }

    // 沉浸式：隐藏系统状态栏/标题栏，扫码期间全屏；退出时恢复。
    DisposableEffect(context) {
        val window = (context as? Activity)?.window
        val controller = window?.let { WindowCompat.getInsetsController(it, it.decorView) }
        controller?.systemBarsBehavior =
            WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        controller?.hide(WindowInsetsCompat.Type.systemBars())
        onDispose {
            controller?.show(WindowInsetsCompat.Type.systemBars())
        }
    }

    // 返回手势与左上角关闭按钮均调用 onClose 退出扫码。
    BackHandler(onBack = onClose)

    DisposableEffect(cameraPermissionGranted, lifecycleOwner, previewView) {
        if (!cameraPermissionGranted) {
            return@DisposableEffect onDispose { }
        }

        val cameraProviderFuture = ProcessCameraProvider.getInstance(context)
        val preview = Preview.Builder().build().apply {
            setSurfaceProvider(previewView.surfaceProvider)
        }
        val processingFrame = AtomicBoolean(false)
        // 分析线程串行回调内的统计（单线程 executor，无需原子量）。
        var firstFrame = true
        var framesSinceReport = 0
        var decodeNanosSinceReport = 0L
        val analysis = ImageAnalysis.Builder()
            // 默认分析流仅 640×480：二维码稍远时单模块像素不足，ZXing 解不出。
            // 目标 1080p，取最接近可用分辨率。
            .setResolutionSelector(
                ResolutionSelector.Builder()
                    .setResolutionStrategy(
                        ResolutionStrategy(
                            android.util.Size(1920, 1080),
                            ResolutionStrategy.FALLBACK_RULE_CLOSEST_HIGHER_THEN_LOWER
                        )
                    )
                    .build()
            )
            .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
            .build()
            .apply {
                setAnalyzer(cameraExecutor) { imageProxy ->
                    if (hasScanned.get()) {
                        imageProxy.close()
                        return@setAnalyzer
                    }
                    if (!processingFrame.compareAndSet(false, true)) {
                        imageProxy.close()
                        return@setAnalyzer
                    }
                    try {
                        if (firstFrame) {
                            firstFrame = false
                            Log.i(
                                TAG,
                                "analysis first frame ${imageProxy.width}x${imageProxy.height}" +
                                    " rot=${imageProxy.imageInfo.rotationDegrees}" +
                                    " format=${imageProxy.format}"
                            )
                        }
                        val started = System.nanoTime()
                        val payload = decodeQrPayload(imageProxy)
                        decodeNanosSinceReport += System.nanoTime() - started
                        if (++framesSinceReport >= REPORT_FRAMES) {
                            val avgMs = decodeNanosSinceReport / framesSinceReport / 1_000_000
                            Log.d(TAG, "analysis $framesSinceReport frames, avg decode ${avgMs}ms")
                            framesSinceReport = 0
                            decodeNanosSinceReport = 0
                        }
                        if (payload != null) {
                            Log.i(TAG, "decoded payload len=${payload.length}")
                            if (hasScanned.compareAndSet(false, true)) {
                                mainExecutor.execute { onPayloadScanned(payload) }
                            }
                        }
                    } catch (throwable: Throwable) {
                        val message = throwable.message
                            ?.let { raw -> "$scanFailedMessage（$raw）" }
                            ?: scanFailedMessage
                        mainExecutor.execute { scanError = message }
                    } finally {
                        processingFrame.set(false)
                        imageProxy.close()
                    }
                }
            }

        val listener = Runnable {
            try {
                val cameraProvider = cameraProviderFuture.get()
                cameraProvider.unbindAll()
                val camera = cameraProvider.bindToLifecycle(
                    lifecycleOwner,
                    CameraSelector.DEFAULT_BACK_CAMERA,
                    preview,
                    analysis
                )
                // 近距二维码容易让连续对焦拉风箱：绑定成功后触发一次中心
                // AF/AE 收敛，之后回落常规连续对焦。
                previewView.post {
                    val cx = previewView.width / 2f
                    val cy = previewView.height / 2f
                    if (cx > 0f && cy > 0f) {
                        camera.cameraControl.startFocusAndMetering(
                            FocusMeteringAction.Builder(
                                previewView.meteringPointFactory.createPoint(cx, cy)
                            ).build()
                        )
                    }
                }
            } catch (throwable: Throwable) {
                scanError = throwable.message
                    ?.let { raw -> "$cameraFailedMessage（$raw）" }
                    ?: cameraFailedMessage
            }
        }
        cameraProviderFuture.addListener(listener, mainExecutor)

        onDispose {
            analysis.clearAnalyzer()
            if (cameraProviderFuture.isDone) {
                runCatching {
                    cameraProviderFuture.get().unbindAll()
                }
            }
        }
    }

    // 全屏相机体验：scrim 角色遮罩 + 上部四角括号取景框 + 提示胶囊 + 底部错误提示。
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.scrim)
    ) {
        if (cameraPermissionGranted) {
            AndroidView(
                factory = { previewView },
                modifier = Modifier.fillMaxSize()
            )
            // 取景层：全屏四角括号（分析管线本就解整帧，无小框限制），
            // 提示胶囊居中悬浮。
            QrReticle(
                modifier = Modifier
                    .fillMaxSize()
                    .statusBarsPadding()
                    .navigationBarsPadding()
                    .padding(horizontal = Spacing.xl, vertical = Spacing.xl)
            )
            Surface(
                modifier = Modifier.align(Alignment.Center),
                shape = CircleShape,
                color = MaterialTheme.colorScheme.inverseSurface.copy(alpha = 0.72f),
                contentColor = MaterialTheme.colorScheme.inverseOnSurface
            ) {
                Text(
                    text = stringResource(R.string.scanner_hint),
                    style = MaterialTheme.typography.bodyMedium,
                    modifier = Modifier.padding(horizontal = Spacing.lg, vertical = Spacing.sm)
                )
            }
        } else {
            Column(
                modifier = Modifier
                    .align(Alignment.Center)
                    .fillMaxWidth()
                    .padding(horizontal = Spacing.xl),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(Spacing.md)
            ) {
                Surface(
                    shape = MaterialTheme.shapes.medium,
                    color = MaterialTheme.colorScheme.surfaceContainer
                ) {
                    Column(
                        modifier = Modifier.padding(Spacing.xl),
                        horizontalAlignment = Alignment.CenterHorizontally,
                        verticalArrangement = Arrangement.spacedBy(Spacing.md)
                    ) {
                        // 64dp tonal 圆形 + 相机图标。
                        Box(
                            modifier = Modifier
                                .size(64.dp)
                                .clip(CircleShape)
                                .background(MaterialTheme.colorScheme.primaryContainer),
                            contentAlignment = Alignment.Center
                        ) {
                            Icon(
                                imageVector = Icons.Outlined.PhotoCamera,
                                contentDescription = null,
                                tint = MaterialTheme.colorScheme.onPrimaryContainer,
                                modifier = Modifier.size(28.dp)
                            )
                        }
                        Text(
                            text = stringResource(R.string.scanner_permission_required),
                            style = MaterialTheme.typography.titleMedium,
                            color = MaterialTheme.colorScheme.onSurface
                        )
                        Text(
                            text = stringResource(R.string.scanner_permission_description),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            textAlign = TextAlign.Center
                        )
                        Button(
                            onClick = { requestPermissionLauncher.launch(Manifest.permission.CAMERA) },
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            Text(stringResource(R.string.scanner_grant_permission))
                        }
                        // 永久拒绝后系统弹窗不再出现：提供直达应用详情页的手动入口。
                        TextButton(
                            onClick = {
                                runCatching {
                                    context.startActivity(
                                        Intent(
                                            Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                                            Uri.fromParts("package", context.packageName, null)
                                        )
                                    )
                                }
                            },
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            Text(stringResource(R.string.scanner_open_settings))
                        }
                    }
                }
            }
        }

        // 可见退出入口（§9.3 幽灵 IconButton）：沉浸模式隐藏了系统栏，返回手势之外
        // 再给一个明确的关闭按钮；仅 UI 覆盖层，相机/分析管线不受影响。
        IconButton(
            onClick = onClose,
            modifier = Modifier
                .align(Alignment.TopStart)
                .statusBarsPadding()
                .padding(Spacing.lg)
        ) {
            Icon(
                imageVector = Icons.Outlined.Close,
                contentDescription = stringResource(R.string.scanner_close),
                tint = Color.White.copy(alpha = 0.9f),
                modifier = Modifier.size(20.dp)
            )
        }

        Column(
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .navigationBarsPadding()
                .padding(bottom = Spacing.xxl),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(Spacing.md)
        ) {
            scanError?.let { error ->
                Surface(
                    shape = CircleShape,
                    color = MaterialTheme.colorScheme.errorContainer,
                    contentColor = MaterialTheme.colorScheme.onErrorContainer
                ) {
                    Row(
                        modifier = Modifier.padding(
                            start = Spacing.md,
                            end = Spacing.lg,
                            top = Spacing.sm,
                            bottom = Spacing.sm
                        ),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Icon(
                            imageVector = Icons.Outlined.ErrorOutline,
                            contentDescription = null,
                            modifier = Modifier.size(16.dp)
                        )
                        Spacer(modifier = Modifier.width(Spacing.sm))
                        Text(
                            text = error,
                            style = MaterialTheme.typography.bodySmall
                        )
                    }
                }
            }
        }
    }
}

private fun decodeQrPayload(imageProxy: ImageProxy): String? {
    val plane = imageProxy.planes.firstOrNull() ?: return null
    val yPlane = QrPayloadDecoder.compactYPlane(
        buffer = plane.buffer,
        width = imageProxy.width,
        height = imageProxy.height,
        rowStride = plane.rowStride,
        pixelStride = plane.pixelStride
    )
    return QrPayloadDecoder.decodeYPlane(
        yPlane = yPlane,
        width = imageProxy.width,
        height = imageProxy.height,
        rotationDegrees = imageProxy.imageInfo.rotationDegrees
    )
}

/**
 * 取景框（纯装饰）：铺满可用区域的四角括号——白 90%、3dp 描边、臂长 36dp，
 * 折弯处经 cornerPathEffect 圆润过渡。表达"整个屏幕都是扫描区"，无交互语义。
 */
@Composable
private fun QrReticle(modifier: Modifier = Modifier) {
    val bracketColor = Color.White.copy(alpha = 0.9f)
    Canvas(modifier = modifier) {
        val bracketStroke = Stroke(
            width = 3.dp.toPx(),
            cap = StrokeCap.Round,
            pathEffect = PathEffect.cornerPathEffect(8.dp.toPx())
        )
        val inset = bracketStroke.width / 2f + 1.dp.toPx()
        val arm = 36.dp.toPx()
        val w = size.width
        val h = size.height
        val path = Path().apply {
            moveTo(inset, inset + arm)
            lineTo(inset, inset)
            lineTo(inset + arm, inset)

            moveTo(w - inset - arm, inset)
            lineTo(w - inset, inset)
            lineTo(w - inset, inset + arm)

            moveTo(w - inset, h - inset - arm)
            lineTo(w - inset, h - inset)
            lineTo(w - inset - arm, h - inset)

            moveTo(inset + arm, h - inset)
            lineTo(inset, h - inset)
            lineTo(inset, h - inset - arm)
        }
        drawPath(
            path = path,
            color = bracketColor,
            style = bracketStroke
        )
    }
}
