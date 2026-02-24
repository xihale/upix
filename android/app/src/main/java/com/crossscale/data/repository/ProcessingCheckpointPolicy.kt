package com.crossscale.data.repository

internal fun ProcessingCheckpoint.pendingUriSet(): Set<String> {
    return pendingItems
        .map { item -> item.uri }
        .filter { uri -> uri.isNotBlank() }
        .toSet()
}

internal fun ProcessingCheckpoint.failedPendingUriSet(): Set<String> {
    val pendingUris = pendingUriSet()
    if (pendingUris.isEmpty()) {
        return emptySet()
    }
    return failureDetails
        .map { detail -> detail.uri }
        .filter { uri -> uri in pendingUris }
        .toSet()
}

internal fun ProcessingCheckpoint.failedPendingCount(): Int {
    return failedPendingUriSet().size
}

internal fun ProcessingCheckpoint.canRetryFailedOnly(): Boolean {
    return failedPendingCount() > 0
}
