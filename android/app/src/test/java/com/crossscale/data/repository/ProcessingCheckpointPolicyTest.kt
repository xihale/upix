package com.crossscale.data.repository

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ProcessingCheckpointPolicyTest {
    @Test
    fun canRetryFailedOnlyReturnsFalseWhenPendingIsEmpty() {
        val checkpoint = checkpointOf(
            pendingUris = emptyList(),
            failedUris = listOf("uri://a")
        )

        assertFalse(checkpoint.canRetryFailedOnly())
        assertEquals(0, checkpoint.failedPendingCount())
    }

    @Test
    fun canRetryFailedOnlyReturnsFalseWhenFailuresAreNotPending() {
        val checkpoint = checkpointOf(
            pendingUris = listOf("uri://a", "uri://b"),
            failedUris = listOf("uri://x")
        )

        assertFalse(checkpoint.canRetryFailedOnly())
        assertEquals(0, checkpoint.failedPendingCount())
    }

    @Test
    fun canRetryFailedOnlyReturnsTrueWhenAnyFailedItemIsPending() {
        val checkpoint = checkpointOf(
            pendingUris = listOf("uri://a", "uri://b"),
            failedUris = listOf("uri://b")
        )

        assertTrue(checkpoint.canRetryFailedOnly())
        assertEquals(1, checkpoint.failedPendingCount())
    }

    @Test
    fun failedPendingCountIgnoresBlankAndDuplicateUris() {
        val checkpoint = checkpointOf(
            pendingUris = listOf("uri://a", "", "uri://a", "uri://b"),
            failedUris = listOf("uri://a", "uri://a", "", "uri://c")
        )

        assertTrue(checkpoint.canRetryFailedOnly())
        assertEquals(1, checkpoint.failedPendingCount())
    }

    private fun checkpointOf(
        pendingUris: List<String>,
        failedUris: List<String>
    ): ProcessingCheckpoint {
        return ProcessingCheckpoint(
            taskId = "task-1",
            source = "source",
            modelName = "model",
            distributedMode = false,
            totalItems = pendingUris.size,
            completedItems = 0,
            outputDirectory = "/tmp/out",
            createdAtEpochMs = 1L,
            updatedAtEpochMs = 1L,
            pendingItems = pendingUris.mapIndexed { index, uri ->
                ProcessingPendingItem(
                    uri = uri,
                    displayName = "pending-$index"
                )
            },
            failureDetails = failedUris.mapIndexed { index, uri ->
                ProcessingFailureDetail(
                    uri = uri,
                    displayName = "failed-$index",
                    errorMessage = "error-$index"
                )
            }
        )
    }
}
