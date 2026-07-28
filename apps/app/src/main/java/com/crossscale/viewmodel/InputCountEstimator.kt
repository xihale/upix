package com.crossscale.viewmodel

import com.crossscale.data.repository.InputSourceResolver
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

// Estimates how many inputs a source path contains, for live UI preview before
// a run starts. Holds its own request-id + cancellable job; the only thing it
// cannot own is the UI state itself, so it reports transitions via two callbacks.
//
// - onEstimatingStarted: set isEstimatingInputCount=true (preserve existing
//   expectedTotalItems, matching the original VM behavior).
// - onResult: set expectedTotalItems + isEstimatingInputCount=false. Also used
//   for the blank-path reset (onResult(null, false)).
class InputCountEstimator(
    private val inputSourceRepository: InputSourceResolver,
    private val scope: CoroutineScope,
    private val ioDispatcher: CoroutineDispatcher,
    private val onEstimatingStarted: () -> Unit,
    private val onResult: (Int?, Boolean) -> Unit,
) {
    private var previewCountJob: Job? = null
    private var previewRequestId: Long = 0L

    fun refreshExpectedTotal(imagePath: String?) {
        val normalizedImagePath = imagePath?.trim().orEmpty()
        previewRequestId += 1
        val requestId = previewRequestId
        previewCountJob?.cancel()

        if (normalizedImagePath.isBlank()) {
            onResult(null, false)
            return
        }

        onEstimatingStarted()

        previewCountJob = scope.launch {
            val expectedTotal = withContext(ioDispatcher) {
                val totalImages =
                    inputSourceRepository.resolveImageInputs(source = normalizedImagePath, nested = true).size
                if (totalImages <= 0) {
                    null
                } else {
                    totalImages
                }
            }
            if (!isActive || requestId != previewRequestId) {
                return@launch
            }
            onResult(expectedTotal, false)
        }
    }

    fun cancel() {
        previewCountJob?.cancel()
    }
}
