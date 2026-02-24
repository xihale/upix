package com.crossscale.network

import java.net.URI
import java.net.URLDecoder
import java.nio.charset.StandardCharsets
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonObject

data class JoinInvite(
    val host: String,
    val port: Int,
    val authKey: String
)

object JoinInviteParser {
    private const val defaultPort = 9527

    fun parse(rawValue: String): JoinInvite? {
        val value = rawValue.trim()
        if (value.isEmpty()) {
            return null
        }

        if (value.startsWith("{")) {
            parseJsonInvite(value)?.let { return it }
        }

        if (value.startsWith("crossscale://", ignoreCase = true)) {
            return parseUriInvite(value)
        }

        parseShortInvite(value)?.let { return it }
        parseLegacyInvite(value)?.let { return it }

        return null
    }

    private fun parseJsonInvite(rawJson: String): JoinInvite? {
        return runCatching {
            val json = Json.parseToJsonElement(rawJson).jsonObject
            val host = json.readString("ip")
                .ifBlank { json.readString("host") }
                .trim()
            if (host.isEmpty()) {
                return null
            }
            val port = json.readInt("port") ?: defaultPort
            if (port !in 1..65535) {
                return null
            }
            val authKey = json.readString("key")
                .ifBlank { json.readString("auth_key") }
                .trim()
            JoinInvite(host = host, port = port, authKey = authKey)
        }.getOrNull()
    }

    private fun parseShortInvite(value: String): JoinInvite? {
        val hashIndex = value.indexOf('#')
        if (hashIndex <= 0) {
            return null
        }
        val hostPort = value.substring(0, hashIndex).trim()
        val authKey = value.substring(hashIndex + 1).trim()
        val portSplit = splitHostPort(hostPort) ?: return null
        return JoinInvite(host = portSplit.first, port = portSplit.second, authKey = authKey)
    }

    private fun parseLegacyInvite(value: String): JoinInvite? {
        val parts = value.split(":", limit = 3)
        if (parts.size < 2) {
            return null
        }
        val host = parts[0].trim()
        val port = parts[1].trim().toIntOrNull() ?: return null
        if (host.isEmpty() || port !in 1..65535) {
            return null
        }
        val authKey = parts.getOrNull(2)?.trim().orEmpty()
        return JoinInvite(host = host, port = port, authKey = authKey)
    }

    private fun parseUriInvite(uriRaw: String): JoinInvite? {
        val uri = runCatching { URI(uriRaw) }.getOrNull() ?: return null
        val params = parseQueryParams(uri.rawQuery.orEmpty())

        val host = params["host"]
            ?.trim()
            .orEmpty()
            .ifBlank { uri.host?.trim().orEmpty() }
        if (host.isEmpty()) {
            return null
        }

        val port = params["port"]
            ?.toIntOrNull()
            ?: uri.port.takeIf { it > 0 }
            ?: defaultPort
        if (port !in 1..65535) {
            return null
        }
        val authKey = params["key"].orEmpty().trim()
        return JoinInvite(host = host, port = port, authKey = authKey)
    }

    private fun splitHostPort(value: String): Pair<String, Int>? {
        val colonIndex = value.lastIndexOf(':')
        if (colonIndex <= 0 || colonIndex == value.lastIndex) {
            return null
        }
        val host = value.substring(0, colonIndex).trim()
        val port = value.substring(colonIndex + 1).trim().toIntOrNull() ?: return null
        if (host.isEmpty() || port !in 1..65535) {
            return null
        }
        return host to port
    }

    private fun parseQueryParams(query: String): Map<String, String> {
        if (query.isBlank()) {
            return emptyMap()
        }
        return query.split("&")
            .mapNotNull { pair ->
                val eq = pair.indexOf('=')
                if (eq <= 0) {
                    return@mapNotNull null
                }
                val key = pair.substring(0, eq)
                if (key.isBlank()) {
                    return@mapNotNull null
                }
                val value = pair.substring(eq + 1)
                val decoded = URLDecoder.decode(value, StandardCharsets.UTF_8)
                key to decoded
            }
            .toMap()
    }

    private fun JsonObject.readString(key: String): String {
        val value = this[key] as? JsonPrimitive ?: return ""
        return value.contentOrNull.orEmpty()
    }

    private fun JsonObject.readInt(key: String): Int? {
        val value = this[key] ?: return null
        return when (value) {
            is JsonPrimitive -> value.intOrNull
            else -> null
        }
    }
}
