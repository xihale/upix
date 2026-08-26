package io.upix.data.repository

import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonPrimitive

object InputSourceSpec {
    private const val MULTI_SOURCE_PREFIX = "upix://multi:"

    fun encode(sources: List<String>): String? {
        val normalizedSources = normalizeSources(sources)
        if (normalizedSources.isEmpty()) {
            return null
        }
        if (normalizedSources.size == 1) {
            return normalizedSources.first()
        }
        val payload = buildJsonArray {
            normalizedSources.forEach { source ->
                add(JsonPrimitive(source))
            }
        }
        return MULTI_SOURCE_PREFIX + Json.encodeToString(JsonArray.serializer(), payload)
    }

    fun decode(source: String): List<String>? {
        if (!source.startsWith(MULTI_SOURCE_PREFIX)) {
            return null
        }
        val payload = source.removePrefix(MULTI_SOURCE_PREFIX).trim()
        if (payload.isBlank()) {
            return emptyList()
        }
        val parsed = runCatching {
            Json.parseToJsonElement(payload) as? JsonArray
        }.getOrNull() ?: return emptyList()
        return normalizeSources(
            parsed.mapNotNull { element ->
                element.jsonPrimitive.contentOrNull
            }
        )
    }

    private fun normalizeSources(sources: List<String>): List<String> {
        return sources
            .map { source -> source.trim() }
            .filter { source -> source.isNotBlank() }
            .distinct()
    }
}
