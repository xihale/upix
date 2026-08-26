package io.upix

import android.content.Context
import io.upix.data.repository.AppSettingsRepository
import io.upix.data.repository.BatchOutputRepository
import io.upix.data.repository.DeviceRepository
import io.upix.data.repository.InputSourceRepository
import io.upix.data.repository.LogRepository
import io.upix.data.repository.ModelReleaseRepository
import io.upix.data.repository.ModelRepository
import io.upix.data.repository.NativeImageProcessingRepository
import io.upix.data.repository.PairingRepository
import io.upix.data.repository.ProcessingCheckpointRepository
import io.upix.data.repository.ProcessingQueueRepository
import io.upix.network.CoordinatorClient
import io.upix.util.StringProvider
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
