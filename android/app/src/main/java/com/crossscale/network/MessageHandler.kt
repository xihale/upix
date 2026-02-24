package com.crossscale.network

import com.crossscale.data.repository.LogRepository

class MessageHandler(
    private val logRepository: LogRepository
) {
    fun onMessageReceived(rawMessage: String) {
        if (rawMessage.isBlank()) {
            return
        }
        logRepository.debug("MessageHandler", "Received message: $rawMessage")
    }
}
