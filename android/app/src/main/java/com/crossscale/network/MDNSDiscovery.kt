package com.crossscale.network

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.net.wifi.WifiManager
import android.util.Log

data class DiscoveredService(
    val name: String,
    val host: String,
    val port: Int
)

class MDNSDiscovery(
    context: Context
) {
    private val appContext = context.applicationContext
    private val nsdManager = appContext.getSystemService(Context.NSD_SERVICE) as NsdManager
    private val wifiManager = appContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
    private var discoveryListener: NsdManager.DiscoveryListener? = null
    private var multicastLock: WifiManager.MulticastLock? = null

    private companion object {
        private const val serviceType = "_crossscale._tcp."
    }

    fun start(
        onServiceFound: (DiscoveredService) -> Unit,
        onError: (String) -> Unit
    ) {
        stop()
        discoveryListener = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(regType: String) {
                Log.i("MDNSDiscovery", "Discovery started: $regType")
            }

            override fun onServiceFound(serviceInfo: NsdServiceInfo) {
                val normalizedType = serviceInfo.serviceType.trim().lowercase()
                if (normalizedType != serviceType && normalizedType != serviceType.removeSuffix(".")) {
                    return
                }
                nsdManager.resolveService(serviceInfo, object : NsdManager.ResolveListener {
                    override fun onResolveFailed(serviceInfo: NsdServiceInfo, errorCode: Int) {
                        onError("Resolve failed: $errorCode")
                    }

                    override fun onServiceResolved(resolvedInfo: NsdServiceInfo) {
                        val hostAddress = resolvedInfo.host?.hostAddress ?: return
                        onServiceFound(
                            DiscoveredService(
                                name = resolvedInfo.serviceName,
                                host = hostAddress,
                                port = resolvedInfo.port
                            )
                        )
                    }
                })
            }

            override fun onServiceLost(serviceInfo: NsdServiceInfo) {
                Log.w("MDNSDiscovery", "Service lost: ${serviceInfo.serviceName}")
            }

            override fun onDiscoveryStopped(serviceType: String) {
                Log.i("MDNSDiscovery", "Discovery stopped: $serviceType")
            }

            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) {
                onError("Discovery start failed: $errorCode")
                nsdManager.stopServiceDiscovery(this)
            }

            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) {
                onError("Discovery stop failed: $errorCode")
                nsdManager.stopServiceDiscovery(this)
            }
        }

        acquireMulticastLockIfNeeded()
        nsdManager.discoverServices(
            serviceType,
            NsdManager.PROTOCOL_DNS_SD,
            discoveryListener
        )
    }

    fun stop() {
        val activeListener = discoveryListener ?: return
        try {
            nsdManager.stopServiceDiscovery(activeListener)
        } catch (_: Exception) {
            // Ignore duplicate stop invocations.
        }
        discoveryListener = null
        releaseMulticastLock()
    }

    private fun acquireMulticastLockIfNeeded() {
        if (multicastLock?.isHeld == true) {
            return
        }
        val lock = wifiManager?.createMulticastLock("crossscale-mdns") ?: return
        lock.setReferenceCounted(true)
        runCatching { lock.acquire() }
            .onSuccess { multicastLock = lock }
            .onFailure { Log.w("MDNSDiscovery", "Failed to acquire multicast lock: ${it.message}") }
    }

    private fun releaseMulticastLock() {
        val lock = multicastLock ?: return
        runCatching {
            if (lock.isHeld) {
                lock.release()
            }
        }
        multicastLock = null
    }
}
