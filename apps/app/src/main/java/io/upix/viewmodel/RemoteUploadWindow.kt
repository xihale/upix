package io.upix.viewmodel

/**
 * Scheduling arithmetic for the remote submit-ahead window (one image batch on a
 * persistent distributed session).
 *
 * The PC answers the CLIENT_SUBMIT frames of a session in submission order —
 * `nativeReceiveDistributedResult` only ever returns the *next* frame — so the
 * phone must keep a positional mapping between the task it awaits and the input
 * that task was built from. This class owns that mapping: [inFlight] is always
 * the index of the input whose result the upcoming round receives, and the
 * caller keeps one queued upload per index reported by [uploadsForRound] (in
 * the same order) so the await never picks up a neighbour's result.
 *
 * Item 0 is uploaded by the caller before the first round (handshake/pairing
 * happens on that first submit), therefore the window starts at index 1. Depths:
 *
 * - `aheadDepth == 0` (old PC, CONNECT_RESPONSE without pipeline_depth): strict
 *   serial — [uploadsForRound] launches nothing and the successor is uploaded
 *   only after its predecessor's result arrived (`uploadedAhead = false`).
 * - `aheadDepth == 1`: upload N+1 while awaiting N.
 * - `aheadDepth == 2`: upload N+1 and N+2 while awaiting N.
 */
internal class RemoteUploadWindow(
    val aheadDepth: Int,
    private val itemCount: Int
) {
    /** The successor the caller must await after the current round's result. */
    internal data class Successor(
        val index: Int,
        /** true = already uploaded through [uploadsForRound]; false = upload it now. */
        val uploadedAhead: Boolean
    )

    private val queued = ArrayDeque<Int>()

    // Index of the next input that still needs an upload. Item 0 was uploaded
    // by the caller before the first round, hence the starting value of 1.
    private var nextUpload = if (itemCount > 0) 1 else 0

    /** Index of the input whose result the current round awaits. */
    var inFlight: Int = 0
        private set

    /**
     * Indices to upload *before* awaiting the result of [round]: keeps at most
     * [aheadDepth] inputs uploaded-but-unreceived beyond the in-flight one.
     * Call order of the returned indices is the wire order the caller must keep.
     */
    fun uploadsForRound(round: Int): List<Int> {
        val uploads = mutableListOf<Int>()
        while (nextUpload < itemCount && nextUpload < round + 1 + aheadDepth) {
            uploads += nextUpload
            queued += nextUpload
            nextUpload += 1
        }
        return uploads
    }

    /**
     * Advances to the task awaited after [round]'s result arrived: the head of
     * the submit-ahead queue when one is queued, otherwise [Successor.index]
     * with `uploadedAhead = false` (the caller uploads it then — depth 0, or the
     * window's tail). Returns null once every item has been awaited.
     */
    fun advanceAfterResult(round: Int): Successor? {
        queued.removeFirstOrNull()?.let { queuedIndex ->
            inFlight = queuedIndex
            return Successor(queuedIndex, uploadedAhead = true)
        }
        val serialIndex = round + 1
        if (serialIndex >= itemCount) {
            return null
        }
        inFlight = serialIndex
        // The serial upload stands in for the missing ahead upload: move the
        // upload cursor past it so uploadsForRound never re-uploads it.
        if (nextUpload <= serialIndex) {
            nextUpload = serialIndex + 1
        }
        return Successor(serialIndex, uploadedAhead = false)
    }
}
