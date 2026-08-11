package com.crossscale

import android.content.Context
import com.crossscale.data.repository.BatchOutputRepository
import com.crossscale.data.repository.DeviceRepository
import com.crossscale.data.repository.InputSourceRepository
import com.crossscale.data.repository.LogRepository
import com.crossscale.data.repository.ModelRepository
import com.crossscale.data.repository.NativeImageProcessingRepository
import com.crossscale.data.repository.PairingRepository
import com.crossscale.data.repository.ProcessingCheckpointRepository
import com.crossscale.data.repository.ProcessingQueueRepository
import com.crossscale.network.CoordinatorClient
import com.crossscale.network.MDNSDiscovery

class AppContainer(context: Context) {
    val appContext = context.applicationContext

    val logRepository = LogRepository()
    val modelRepository = ModelRepository(appContext, logRepository)
    val deviceRepository = DeviceRepository(appContext, logRepository)
    val pairingRepository = PairingRepository(appContext, logRepository)
    val inputSourceRepository = InputSourceRepository(appContext, logRepository)
    val batchOutputRepository = BatchOutputRepository(appContext)
    val processingCheckpointRepository = ProcessingCheckpointRepository(appContext, logRepository)
    val processingQueueRepository = ProcessingQueueRepository(appContext, logRepository)
    val nativeImageProcessingRepository = NativeImageProcessingRepository(appContext, logRepository)
    val coordinatorClient = CoordinatorClient(appContext, logRepository)
    val mdnsDiscovery = MDNSDiscovery(appContext)
}
