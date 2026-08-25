package com.upix

import android.content.Context
import com.upix.data.repository.AppSettingsRepository
import com.upix.data.repository.BatchOutputRepository
import com.upix.data.repository.DeviceRepository
import com.upix.data.repository.InputSourceRepository
import com.upix.data.repository.LogRepository
import com.upix.data.repository.ModelReleaseRepository
import com.upix.data.repository.ModelRepository
import com.upix.data.repository.NativeImageProcessingRepository
import com.upix.data.repository.PairingRepository
import com.upix.data.repository.ProcessingCheckpointRepository
import com.upix.data.repository.ProcessingQueueRepository
import com.upix.network.CoordinatorClient
import com.upix.util.StringProvider
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers

class AppContainer(context: Context) {
    val appContext = context.applicationContext

    /** 容器统一的 IO 调度器：所有仓库的文件 IO 注入点，单测可整体替换。 */
    val ioDispatcher: CoroutineDispatcher = Dispatchers.IO

    val logRepository = LogRepository()
    val stringProvider = StringProvider { resId, formatArgs ->
        appContext.getString(resId, *formatArgs)
    }
    val appSettingsRepository = AppSettingsRepository(appContext)
    val modelRepository = ModelRepository(appContext, logRepository)
    val modelReleaseRepository = ModelReleaseRepository(logRepository)
    val deviceRepository = DeviceRepository(logRepository)
    val pairingRepository = PairingRepository(appContext, logRepository)
    val inputSourceRepository = InputSourceRepository(appContext, logRepository)
    val batchOutputRepository = BatchOutputRepository(appContext)
    val processingCheckpointRepository = ProcessingCheckpointRepository(appContext, logRepository, ioDispatcher)
    val processingQueueRepository = ProcessingQueueRepository(appContext, logRepository, ioDispatcher)
    val nativeImageProcessingRepository = NativeImageProcessingRepository(appContext, logRepository)
    val coordinatorClient = CoordinatorClient(appContext, logRepository)
}
