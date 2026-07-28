package com.crossscale.viewmodel

import com.crossscale.data.repository.BatchOutputStore
import com.crossscale.data.repository.InputSourceResolver
import com.crossscale.data.repository.LogRepository
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

// Owns backup inspection/restoration. The only repositories it touches are
// inputSourceRepository and batchOutputRepository; it has no view-state and no
// access to the processing job, so it is fully independent of the VM core.
class BackupRestoreManager(
    private val inputSourceRepository: InputSourceResolver,
    private val batchOutputRepository: BatchOutputStore,
    private val logRepository: LogRepository,
    private val scope: CoroutineScope,
    private val ioDispatcher: CoroutineDispatcher,
) {
    suspend fun hasBackupFor(imagePath: String, nested: Boolean): Boolean = withContext(ioDispatcher) {
        val inputs = inputSourceRepository.resolveImageInputs(source = imagePath, nested = nested)
        inputs.any { input ->
            batchOutputRepository.hasBackup(input)
        }
    }

    fun restoreBackup(imagePath: String, nested: Boolean, onComplete: (Int, Int) -> Unit) {
        scope.launch {
            var restored = 0
            var failed = 0
            withContext(ioDispatcher) {
                val inputs = inputSourceRepository.resolveImageInputs(source = imagePath, nested = nested)
                for (input in inputs) {
                    if (batchOutputRepository.hasBackup(input)) {
                        batchOutputRepository.restoreBackup(input).fold(
                            onSuccess = { restored++ },
                            onFailure = {
                                failed++
                                logRepository.error("ProcessingViewModel", "Failed to restore backup for ${input.displayName}: ${it.message}")
                            }
                        )
                    }
                }
            }
            if (restored > 0 || failed > 0) {
                logRepository.info("ProcessingViewModel", "Restore completed. Restored: $restored, Failed: $failed")
            }
            onComplete(restored, failed)
        }
    }
}
