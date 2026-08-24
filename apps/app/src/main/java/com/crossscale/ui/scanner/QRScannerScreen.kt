package com.crossscale.ui.scanner

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.provider.Settings
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.camera.core.CameraSelector
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
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.ErrorOutline
import androidx.compose.material.icons.outlined.PhotoCamera
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
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
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.crossscale.R
import com.crossscale.ui.theme.Spacing
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

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

    val cameraExecutor = remember { Executors.newSingleThreadExecutor() }
    val hasScanned = remember { AtomicBoolean(false) }

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

    // 无关闭按钮，返回手势即退出扫码。
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
        val analysis = ImageAnalysis.Builder()
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
                        val payload = decodeQrPayload(imageProxy)
                        if (payload != null && hasScanned.compareAndSet(false, true)) {
                            onPayloadScanned(payload)
                        }
                    } catch (throwable: Throwable) {
                        scanError = throwable.message
                            ?.let { raw -> "$scanFailedMessage（$raw）" }
                            ?: scanFailedMessage
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
                cameraProvider.bindToLifecycle(
                    lifecycleOwner,
                    CameraSelector.DEFAULT_BACK_CAMERA,
                    preview,
                    analysis
                )
            } catch (throwable: Throwable) {
                scanError = throwable.message
                    ?.let { raw -> "$cameraFailedMessage（$raw）" }
                    ?: cameraFailedMessage
            }
        }
        cameraProviderFuture.addListener(listener, ContextCompat.getMainExecutor(context))

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
            // 取景框层（纯装饰）：约 35% 高度偏移处放置四角括号，其下挂提示胶囊。
            Column(
                modifier = Modifier.fillMaxSize(),
                horizontalAlignment = Alignment.CenterHorizontally
            ) {
                Spacer(modifier = Modifier.weight(0.34f))
                QrReticle(modifier = Modifier.size(256.dp))
                Spacer(modifier = Modifier.height(Spacing.xl))
                Surface(
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
                Spacer(modifier = Modifier.weight(0.66f))
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
                        OutlinedButton(
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
 * 取景框（纯装饰）：极淡圆角描边（10% 白）+ 四角括号（白 90%、2dp 描边、
 * 臂长 28dp，折弯处经 cornerPathEffect 圆润过渡）。无交互语义。
 */
@Composable
private fun QrReticle(modifier: Modifier = Modifier) {
    val bracketColor = Color.White.copy(alpha = 0.9f)
    val faintBorderColor = Color.White.copy(alpha = 0.1f)
    Canvas(modifier = modifier) {
        val stroke = Stroke(width = 2.dp.toPx(), cap = StrokeCap.Round)
        val bracketStroke = Stroke(
            width = 2.dp.toPx(),
            cap = StrokeCap.Round,
            pathEffect = PathEffect.cornerPathEffect(8.dp.toPx())
        )
        val inset = stroke.width / 2f + 1.dp.toPx()
        val cornerRadius = CornerRadius(18.dp.toPx(), 18.dp.toPx())

        // 极淡的圆角边框，标出完整扫描区域。
        drawRoundRect(
            color = faintBorderColor,
            topLeft = Offset(inset, inset),
            size = Size(size.width - inset * 2f, size.height - inset * 2f),
            cornerRadius = cornerRadius,
            style = stroke
        )

        val arm = 28.dp.toPx()
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
