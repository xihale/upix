package com.upix.data.model

import androidx.compose.runtime.Immutable

// List fields would otherwise be inferred unstable by the Compose compiler;
// by convention the lists are never mutated after construction.
@Immutable
data class ModelImportResult(
    val importedModelNames: List<String> = emptyList(),
    val invalidItems: List<String> = emptyList()
) {
    val importedCount: Int
        get() = importedModelNames.size

    val invalidCount: Int
        get() = invalidItems.size
}
