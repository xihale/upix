package io.upix.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.wifi.WifiManager
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import io.upix.MainActivity
import io.upix.R
import io.upix.UpixApplication
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch

/**
 * 批处理前台保活服务（FGS type dataSync，keeper 方案）：
 * - 不承载处理逻辑——run 协程仍在 ProcessingViewModel 的 viewModelScope 里，
 *   本服务只负责“处理期间进程不被冻结/杀”+ 前台进度通知 + 完成通知。
 * - 观察 [ProcessingForegroundCoordinator.state]：active=true 存活并低频
 *   （≤1Hz，换文件/换批次立即刷新）更新进度通知；active=false 时
 *   stopForeground + stopSelf。
 * - 收集 [ProcessingForegroundCoordinator.completions]：批次
 *   COMPLETED/FAILED/CANCELED 各发一条通知，点击经 PendingIntent 打开
 *   MainActivity。
 * - 处理期间持有 PARTIAL_WAKE_LOCK（批间也保持，直到 active=false 释放），
 *   onDestroy / onTimeout / onTaskRemoved 都会释放并自停。
 *
 * 用户从最近任务划掉 app：onTaskRemoved 里自停（Activity 销毁 →
 * ViewModelStore 清空 → run 协程随之取消，checkpoint 已由引擎的
 * NonCancellable 收尾落盘），行为明确——划掉即批处理停、断点可续。
 */
class ProcessingKeepAliveService : Service() {

    private lateinit var coordinator: ProcessingForegroundCoordinator
    private lateinit var serviceScope: CoroutineScope

    private var wakeLock: PowerManager.WakeLock? = null

    // PARTIAL_WAKE_LOCK 只保 CPU 不保 Wi-Fi：屏灭后 Wi-Fi 进省电态，厂商电源
    // 管理（HarmonyOS/MIUI）会直接掐掉 TCP——远程批处理“Connection lost while
    // task(s) were running”的根因。与 WakeLock 同生命周期持有低延迟 WifiLock。
    private var wifiLock: WifiManager.WifiLock? = null

    // 通知节流：≥1s 才重发一次进度通知；换文件/换批次/active 翻转立即重发。
    private var lastProgressNotifyAtMs = 0L
    private var lastProgressNotifyKey: String? = null
    private var lastSeenCompletionId = 0L

    override fun onCreate() {
        super.onCreate()
        coordinator = (application as UpixApplication).container.processingForegroundCoordinator
        serviceScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
        createNotificationChannel()

        // 进度：active=true 存活 + 渲染通知；active=false 自停。
        serviceScope.launch {
            coordinator.state.collect { snapshot ->
                if (snapshot.active) {
                    acquireWakeLock()
                    notifyProgress(snapshot)
                } else {
                    stopKeepAlive()
                }
            }
        }
        // 完成事件：一个批次终态一条通知。终态伴随 active=false，进度收集器
        // 随后自停服务，完成通知独立于前台通知（不同 id，stopForeground 不影响）。
        serviceScope.launch {
            coordinator.completions.collect { completion ->
                if (completion.id > lastSeenCompletionId) {
                    lastSeenCompletionId = completion.id
                    notifyCompletion(completion)
                }
            }
        }
    }

    // ServiceCompat 内部按 SDK 分派 type 参数（<29 忽略），常量内联无害。
    @android.annotation.SuppressLint("InlinedApi")
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // 5 秒窗口内必须 startForeground：用协调器当前快照立即挂出前台通知。
        val snapshot = coordinator.state.value
        ServiceCompat.startForeground(
            this,
            NOTIFICATION_ID_PROGRESS,
            buildProgressNotification(snapshot),
            ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC
        )
        if (snapshot.active) {
            acquireWakeLock()
        } else {
            stopKeepAlive()
        }
        // 批处理依附于 ViewModel 生命周期，进程被杀后 run 已死：
        // 重建空服务无意义，不 STICKY。
        return START_NOT_STICKY
    }

    // compileSdk 36+ 中 Service.onBind 为抽象成员：本服务为纯启动式服务。
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onTimeout(startId: Int) {
        // API 34 形参重载（shortService 用）：dataSync 走下方双参重载，这里兜底。
        onTimeout(startId, 0)
    }

    override fun onTimeout(startId: Int, foregroundServiceType: Int) {
        // Android 15+ 的 dataSync 6 小时累计上限：到时自停（run 仍在
        // viewModelScope 里继续跑，只是失去前台保护）。
        Log.w(TAG, "dataSync foreground service timed out (6h quota); stopping keep-alive")
        stopKeepAlive()
    }

    override fun onTaskRemoved(rootIntent: Intent?) {
        super.onTaskRemoved(rootIntent)
        // 划掉最近任务 = 用户显式结束本次会话：自停服务（run 协程随
        // ViewModel 清理而取消，checkpoint 已落盘可续传）。
        stopKeepAlive()
    }

    override fun onDestroy() {
        releaseWakeLock()
        serviceScope.cancel()
        coordinator.onServiceDestroyed()
        super.onDestroy()
    }

    // ---- WakeLock ----

    @android.annotation.SuppressLint("WakelockTimeout")
    private fun acquireWakeLock() {
        val existing = wakeLock
        if (existing?.isHeld == true) {
            return
        }
        val powerManager = getSystemService(Context.POWER_SERVICE) as PowerManager
        val lock = powerManager.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, WAKE_LOCK_TAG)
        lock.setReferenceCounted(false)
        // 无超时持有：生命周期由 active=false / onDestroy 显式释放，
        // 与批处理时长对齐（不受 lint 建议的 10 分钟上限约束）。
        lock.acquire()
        wakeLock = lock
        acquireWifiLock()
    }

    private fun releaseWakeLock() {
        wakeLock?.let { lock ->
            if (lock.isHeld) {
                lock.release()
            }
        }
        wakeLock = null
        releaseWifiLock()
    }

    private fun acquireWifiLock() {
        val existing = wifiLock
        if (existing?.isHeld == true) {
            return
        }
        val wifiManager = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        val mode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            WifiManager.WIFI_MODE_FULL_LOW_LATENCY
        } else {
            @Suppress("DEPRECATION")
            WifiManager.WIFI_MODE_FULL_HIGH_PERF
        }
        val lock = wifiManager.createWifiLock(mode, WIFI_LOCK_TAG)
        lock.setReferenceCounted(false)
        lock.acquire()
        wifiLock = lock
    }

    private fun releaseWifiLock() {
        wifiLock?.let { lock ->
            if (lock.isHeld) {
                lock.release()
            }
        }
        wifiLock = null
    }

    // ---- 自停 ----

    private fun stopKeepAlive() {
        releaseWakeLock()
        ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    // ---- 通知 ----

    private fun createNotificationChannel() {
        val channel = NotificationChannel(
            CHANNEL_ID,
            getString(R.string.notification_channel_processing_name),
            NotificationManager.IMPORTANCE_LOW
        ).apply {
            description = getString(R.string.notification_channel_processing_description)
            setShowBadge(false)
        }
        val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        manager.createNotificationChannel(channel)
    }

    private fun notifyProgress(snapshot: ForegroundProgressSnapshot) {
        val now = SystemClock.elapsedRealtime()
        val key = "${snapshot.runningTaskId}|${snapshot.currentItemName}|${snapshot.active}"
        if (key == lastProgressNotifyKey && now - lastProgressNotifyAtMs < PROGRESS_NOTIFY_MIN_INTERVAL_MS) {
            return
        }
        lastProgressNotifyAtMs = now
        lastProgressNotifyKey = key
        notificationManager().notify(NOTIFICATION_ID_PROGRESS, buildProgressNotification(snapshot))
    }

    private fun buildProgressNotification(snapshot: ForegroundProgressSnapshot): Notification {
        val counts = getString(
            R.string.notification_processing_counts,
            snapshot.completedItems,
            snapshot.totalItems
        )
        val contentText = buildString {
            append(counts)
            snapshot.currentItemName?.takeIf { it.isNotBlank() }?.let { name ->
                append(SEPARATOR)
                append(getString(R.string.notification_processing_current, name))
            }
            snapshot.remoteInferencePercent?.let { percent ->
                append(SEPARATOR)
                append(
                    if (snapshot.remoteEtaSeconds != null && snapshot.remoteEtaSeconds > 0) {
                        getString(
                            R.string.processing_remote_phase_inference_eta,
                            percent,
                            snapshot.remoteEtaSeconds
                        )
                    } else {
                        getString(R.string.processing_remote_phase_inference, percent)
                    }
                )
            }
        }
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_launcher_foreground)
            .setContentTitle(
                getString(
                    R.string.notification_processing_title,
                    snapshot.modelName ?: getString(R.string.app_name)
                )
            )
            .setContentText(contentText)
            .setProgress(snapshot.totalItems, snapshot.completedItems, snapshot.totalItems <= 0)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setSilent(true)
            .setContentIntent(mainActivityPendingIntent())
            .setCategory(NotificationCompat.CATEGORY_PROGRESS)
            .build()
    }

    private fun notifyCompletion(completion: ForegroundCompletionSnapshot) {
        val titleRes = when {
            completion.completed -> R.string.notification_completed_title
            completion.canceled -> R.string.notification_canceled_title
            else -> R.string.notification_failed_title
        }
        val content = when {
            completion.completed -> getString(
                R.string.notification_completed_content,
                completion.completedItems,
                completion.outputDirectory ?: getString(R.string.app_name)
            )

            completion.canceled -> getString(
                R.string.notification_canceled_content,
                completion.completedItems,
                completion.totalItems
            )

            else -> getString(
                R.string.notification_failed_content,
                completion.completedItems,
                completion.totalItems,
                completion.errorMessage ?: getString(R.string.processing_error_unknown_failure)
            )
        }
        val notification = NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_launcher_foreground)
            .setContentTitle(getString(titleRes))
            .setContentText(content)
            .setStyle(NotificationCompat.BigTextStyle().bigText(content))
            .setAutoCancel(true)
            .setContentIntent(mainActivityPendingIntent())
            .build()
        notificationManager().notify(NOTIFICATION_ID_COMPLETION, notification)
    }

    private fun mainActivityPendingIntent(): PendingIntent {
        val intent = Intent(this, MainActivity::class.java).apply {
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        return PendingIntent.getActivity(
            this,
            0,
            intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
    }

    private fun notificationManager(): NotificationManager =
        getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager

    companion object {
        private const val TAG = "ProcessingKeepAlive"
        private const val CHANNEL_ID = "processing_keepalive"
        private const val NOTIFICATION_ID_PROGRESS = 1001
        private const val NOTIFICATION_ID_COMPLETION = 1002
        private const val WAKE_LOCK_TAG = "upix:processing-keepalive"
        private const val WIFI_LOCK_TAG = "upix:processing-wifilock"
        private const val PROGRESS_NOTIFY_MIN_INTERVAL_MS = 1_000L
        private const val SEPARATOR = " · "
    }
}
