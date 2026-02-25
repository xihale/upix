package com.crossscale.data.model

data class ModelImportResult(
    val importedModelNames: List<String> = emptyList(),
    val invalidItems: List<String> = emptyList()
) {
    val importedCount: Int
        get() = importedModelNames.size

    val invalidCount: Int
        get() = invalidItems.size
}
