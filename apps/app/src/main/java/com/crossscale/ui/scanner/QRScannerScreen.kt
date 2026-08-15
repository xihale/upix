package com.crossscale.ui.scanner

import android.Manifest
import android.content.pm.PackageManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.camera.core.CameraSelector
import androidx.camera.core.ExperimentalGetImage
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.Preview
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.view.PreviewView
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Button
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.MaterialTheme
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
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.RoundRect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.ClipOp
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathOperation
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import com.crossscale.R
import com.google.mlkit.vision.barcode.BarcodeScannerOptions
import com.google.mlkit.vision.barcode.BarcodeScanning
import com.google.mlkit.vision.barcode.common.Barcode
import com.google.mlkit.vision.common.InputImage
import com.crossscale.ui.theme.Spacing
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

private val ViewfinderSize = 260.dp
private val ViewfinderCorner = 24.dp
private val ViewfinderStroke = 2.dp
private val CornerStroke = 5.dp
private val CornerLength = 46.dp

@ExperimentalGetImage
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

    val scannerOptions = remember {
        BarcodeScannerOptions.Builder()
            .setBarcodeFormats(Barcode.FORMAT_QR_CODE)
            .build()
    }
    val barcodeScanner = remember { BarcodeScanning.getClient(scannerOptions) }
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
        onDispose {
            cameraExecutor.shutdown()
            barcodeScanner.close()
        }
    }

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

                    val mediaImage = imageProxy.image
                    if (mediaImage == null) {
                        processingFrame.set(false)
                        imageProxy.close()
                        return@setAnalyzer
                    }

                    val image = InputImage.fromMediaImage(
                        mediaImage,
                        imageProxy.imageInfo.rotationDegrees
                    )

                    barcodeScanner.process(image)
                        .addOnSuccessListener { barcodes ->
                            val payload = barcodes
                                .asSequence()
                                .mapNotNull { it.rawValue?.trim() }
                                .firstOrNull { it.isNotEmpty() }

                            if (payload != null && hasScanned.compareAndSet(false, true)) {
                                onPayloadScanned(payload)
                            }
                        }
                        .addOnFailureListener { throwable ->
                            scanError = throwable.message ?: scanFailedMessage
                        }
                        .addOnCompleteListener {
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
                scanError = throwable.message ?: cameraFailedMessage
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

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
    ) {
        if (cameraPermissionGranted) {
            AndroidView(
                factory = { previewView },
                modifier = Modifier.fillMaxSize()
            )
            ViewfinderOverlay(modifier = Modifier.fillMaxSize())

            Column(
                modifier = Modifier
                    .align(Alignment.TopCenter)
                    .fillMaxWidth()
                    .statusBarsPadding()
                    .padding(horizontal = Spacing.lg, vertical = Spacing.md),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(Spacing.sm)
            ) {
                ScannerPill {
                    Text(
                        text = stringResource(R.string.scanner_title),
                        style = MaterialTheme.typography.titleMedium,
                        color = MaterialTheme.colorScheme.onSurface
                    )
                }
                ScannerPill {
                    Text(
                        text = stringResource(R.string.scanner_hint),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurface
                    )
                }
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
                    shape = MaterialTheme.shapes.large,
                    color = MaterialTheme.colorScheme.surfaceContainer
                ) {
                    Column(
                        modifier = Modifier.padding(Spacing.lg),
                        horizontalAlignment = Alignment.CenterHorizontally,
                        verticalArrangement = Arrangement.spacedBy(Spacing.md)
                    ) {
                        Text(
                            text = stringResource(R.string.scanner_permission_required),
                            style = MaterialTheme.typography.bodyMedium
                        )
                        Button(
                            onClick = { requestPermissionLauncher.launch(Manifest.permission.CAMERA) },
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            Text(stringResource(R.string.scanner_grant_permission))
                        }
                    }
                }
            }
        }

        Column(
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .navigationBarsPadding()
                .padding(bottom = Spacing.lg),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(Spacing.md)
        ) {
            scanError?.let { error ->
                Surface(
                    shape = CircleShape,
                    color = MaterialTheme.colorScheme.errorContainer,
                    contentColor = MaterialTheme.colorScheme.onErrorContainer
                ) {
                    Text(
                        text = error,
                        style = MaterialTheme.typography.bodySmall,
                        modifier = Modifier.padding(horizontal = Spacing.md, vertical = Spacing.sm)
                    )
                }
            }
            FilledTonalButton(onClick = onClose) {
                Text(stringResource(R.string.scanner_close))
            }
        }
    }
}

/** 半透明提示 pill：surface.copy(0.6) + 圆角 full + onSurface 字。 */
@Composable
private fun ScannerPill(content: @Composable () -> Unit) {
    Surface(
        shape = CircleShape,
        color = MaterialTheme.colorScheme.surface.copy(alpha = 0.6f),
        contentColor = MaterialTheme.colorScheme.onSurface
    ) {
        Box(modifier = Modifier.padding(horizontal = Spacing.md, vertical = Spacing.xs)) {
            content()
        }
    }
}

/**
 * 取景框：中央 260dp 圆角 24 描边 2dp primary + 四角 L 形加重描边 + 半透明 scrim。
 */
@Composable
private fun ViewfinderOverlay(modifier: Modifier = Modifier) {
    val frameColor = MaterialTheme.colorScheme.primary
    val scrimColor = Color.Black.copy(alpha = 0.55f)

    Canvas(modifier = modifier) {
        val frame = ViewfinderSize.toPx()
        val cornerRadius = ViewfinderCorner.toPx()
        val left = (size.width - frame) / 2f
        val top = (size.height - frame) / 2f
        val right = left + frame
        val bottom = top + frame

        // Scrim: full canvas minus rounded-rect cutout.
        val scrimPath = Path().apply {
            val full = Path().apply {
                addRect(androidx.compose.ui.geometry.Rect(0f, 0f, size.width, size.height))
            }
            val cutout = Path().apply {
                addRoundRect(
                    RoundRect(
                        left = left,
                        top = top,
                        right = right,
                        bottom = bottom,
                        cornerRadius = CornerRadius(cornerRadius, cornerRadius)
                    )
                )
            }
            op(full, cutout, PathOperation.Difference)
        }
        drawPath(path = scrimPath, color = scrimColor)

        // Rounded frame outline.
        drawRoundRect(
            color = frameColor,
            topLeft = Offset(left, top),
            size = Size(frame, frame),
            cornerRadius = CornerRadius(cornerRadius, cornerRadius),
            style = Stroke(width = ViewfinderStroke.toPx())
        )

        // Four heavy L-shaped corner brackets.
        val stroke = CornerStroke.toPx()
        val len = CornerLength.toPx()
        val cap = StrokeCap.Round
        fun bracket(x: Float, y: Float, dx: Float, dy: Float) {
            drawLine(
                color = frameColor,
                start = Offset(x, y),
                end = Offset(x + dx * len, y),
                strokeWidth = stroke,
                cap = cap
            )
            drawLine(
                color = frameColor,
                start = Offset(x, y),
                end = Offset(x, y + dy * len),
                strokeWidth = stroke,
                cap = cap
            )
        }
        bracket(left, top, 1f, 1f)
        bracket(right, top, -1f, 1f)
        bracket(left, bottom, 1f, -1f)
        bracket(right, bottom, -1f, -1f)
    }
}
