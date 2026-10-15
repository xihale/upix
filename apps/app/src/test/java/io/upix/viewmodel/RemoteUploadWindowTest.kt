package io.upix.viewmodel

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Behavior of the remote submit-ahead schedule, simulated the way the run
 * engine drives it: item 0 is uploaded before the first round, each round
 * awaits the oldest outstanding submission, and the window decides which
 * uploads are issued ahead of that await.
 *
 * The invariant under test is the one the PC's session reply order depends on:
 * the task awaited in round N is the task built from input N — never a
 * neighbour's — while never having more than `aheadDepth` submissions on the
 * wire beyond the one being awaited.
 */
class RemoteUploadWindowTest {
    @Test
    fun everyRoundAwaitsItsOwnInputAndUploadsItExactlyOnce() {
        for (depth in 0..2) {
            for (itemCount in 1..6) {
                val schedule = simulate(aheadDepth = depth, itemCount = itemCount)
                assertEquals(
                    "depth=$depth count=$itemCount: rounds",
                    itemCount,
                    schedule.awaitedInputs.size
                )
                assertEquals(
                    "depth=$depth count=$itemCount: awaited order",
                    (0 until itemCount).toList(),
                    schedule.awaitedInputs
                )
                assertEquals(
                    "depth=$depth count=$itemCount: uploaded set",
                    (0 until itemCount).toList(),
                    schedule.uploadedInputs.sorted()
                )
                assertEquals(
                    "depth=$depth count=$itemCount: submissions are strictly ordered",
                    schedule.uploadedInputs,
                    schedule.uploadedInputs.sorted()
                )
            }
        }
    }

    @Test
    fun submitAheadDepthOneUploadsOnlyTheNextInput() {
        val schedule = simulate(aheadDepth = 1, itemCount = 4)

        assertEquals(
            listOf(listOf(1), listOf(2), listOf(3), emptyList()),
            schedule.uploadsBeforeRound
        )
        // Uploaded ahead of the round it belongs to, never the current input.
        assertEquals(listOf(true, true, true), schedule.successorUploadedAhead)
    }

    @Test
    fun deepPipelineDepthTwoKeepsTwoUploadsOnTheWire() {
        val schedule = simulate(aheadDepth = 2, itemCount = 5)

        assertEquals(
            listOf(listOf(1, 2), listOf(3), listOf(4), emptyList(), emptyList()),
            schedule.uploadsBeforeRound
        )
        assertTrue(
            "outstanding submissions beyond the awaited one never exceed the depth",
            schedule.maxOutstanding <= 2
        )
        assertEquals(2, schedule.maxOutstanding)
    }

    @Test
    fun depthZeroIsStrictlySerial() {
        val schedule = simulate(aheadDepth = 0, itemCount = 3)

        assertEquals(
            listOf(emptyList<Int>(), emptyList(), emptyList()),
            schedule.uploadsBeforeRound
        )
        // None of the successors were pre-uploaded: each is uploaded after its
        // predecessor's result arrived (the last round has no successor).
        assertEquals(listOf(false, false), schedule.successorUploadedAhead)
        assertEquals(0, schedule.maxOutstanding)
    }

    @Test
    fun serialWindowNeverReuploadsTheSuccessor() {
        // A window whose ahead-queue runs dry mid-batch must fall back to the
        // serial upload once and not hand the same index out again.
        val window = RemoteUploadWindow(aheadDepth = 0, itemCount = 4)

        assertTrue(window.uploadsForRound(0).isEmpty())
        val first = window.advanceAfterResult(0)
        assertNotNull(first)
        assertEquals(1, first?.index)
        assertFalse(first!!.uploadedAhead)
        // Round 1 must not re-schedule input 1 as an upload.
        assertTrue(window.uploadsForRound(1).isEmpty())
        assertEquals(1, window.inFlight)
    }

    @Test
    fun emptyBatchHasNoRounds() {
        val window = RemoteUploadWindow(aheadDepth = 2, itemCount = 0)

        assertTrue(window.uploadsForRound(0).isEmpty())
        assertEquals(null, window.advanceAfterResult(0))
    }

    private data class Schedule(
        val awaitedInputs: List<Int>,
        val uploadedInputs: List<Int>,
        val uploadsBeforeRound: List<List<Int>>,
        val successorUploadedAhead: List<Boolean>,
        val maxOutstanding: Int
    )

    /**
     * Runs the window through a full batch, mirroring the engine: `submitted`
     * holds the uploads whose results have not been received yet, in wire order
     * (so its head is always the task the round must await).
     */
    private fun simulate(aheadDepth: Int, itemCount: Int): Schedule {
        val window = RemoteUploadWindow(aheadDepth = aheadDepth, itemCount = itemCount)
        val submitted = ArrayDeque<Int>()
        val uploaded = mutableListOf<Int>()
        val awaited = mutableListOf<Int>()
        val uploadsBeforeRound = mutableListOf<List<Int>>()
        val successorUploadedAhead = mutableListOf<Boolean>()
        var maxOutstanding = 0

        fun upload(index: Int) {
            assertFalse("input $index uploaded twice", uploaded.contains(index))
            uploaded += index
            submitted += index
        }

        // The engine submits item 0 before the first round (handshake/pairing).
        upload(0)

        var round = 0
        while (round < itemCount) {
            assertEquals("round $round awaits the wrong input", round, window.inFlight)
            assertEquals("round $round awaits a stale submission", round, submitted.first())

            val uploads = window.uploadsForRound(round)
            uploadsBeforeRound += uploads
            uploads.forEach { upload(it) }
            maxOutstanding = maxOf(maxOutstanding, submitted.size - 1)

            submitted.removeFirst()
            awaited += round

            val successor = window.advanceAfterResult(round) ?: break
            assertEquals("successor of round $round", round + 1, successor.index)
            if (successor.uploadedAhead) {
                assertTrue(
                    "round $round successor ${successor.index} was not pre-uploaded",
                    submitted.contains(successor.index)
                )
            } else {
                assertFalse(
                    "round $round successor ${successor.index} was already uploaded",
                    submitted.contains(successor.index)
                )
                upload(successor.index)
            }
            successorUploadedAhead += successor.uploadedAhead
            round += 1
        }

        assertNotEquals(0, awaited.size)
        assertTrue("batch ended with unreceived submissions: $submitted", submitted.isEmpty())
        return Schedule(
            awaitedInputs = awaited,
            uploadedInputs = uploaded,
            uploadsBeforeRound = uploadsBeforeRound,
            successorUploadedAhead = successorUploadedAhead,
            maxOutstanding = maxOutstanding
        )
    }
}
