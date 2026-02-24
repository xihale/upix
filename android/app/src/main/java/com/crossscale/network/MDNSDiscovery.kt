package com.crossscale.network

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.util.Log

data class DiscoveredService(
    val name: String,
    val host: String,
    val port: Int
)

class MDNSDiscovery(
    context: Context
) {
    private val nsdManager = context.getSystemService(Context.NSD_SERVICE) as NsdManager
    private var discoveryListener: NsdManager.DiscoveryListener? = null

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
                if (serviceInfo.serviceType != "_crossscale._tcp.") {
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

        nsdManager.discoverServices(
            "_crossscale._tcp.",
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
    }
}
