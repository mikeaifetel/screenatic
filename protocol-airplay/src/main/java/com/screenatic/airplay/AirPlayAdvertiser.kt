package com.screenatic.airplay

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.util.Log

/**
 * Publishes `_airplay._tcp` and `_raop._tcp` over mDNS.
 *
 * First pass uses the platform NsdManager. Known issue: TXT records via NsdManager
 * are unreliable on some older Android TV / Fire OS builds. If iPhones don't see
 * the TV, swap this for an embedded responder (e.g. mDNSResponder or jmDNS).
 */
internal class AirPlayAdvertiser(context: Context) {

    private val nsd = context.getSystemService(Context.NSD_SERVICE) as NsdManager
    private val listeners = mutableListOf<NsdManager.RegistrationListener>()

    fun register(
        deviceName: String,
        deviceId: String,
        airplayPort: Int,
        raopPort: Int,
        airplayTxt: Map<String, String>,
        raopTxt: Map<String, String>,
    ) {
        publish(deviceName, "_airplay._tcp", airplayPort, airplayTxt)
        // RAOP instance name is "<deviceid without colons>@<name>"
        publish("${deviceId.replace(":", "")}@$deviceName", "_raop._tcp", raopPort, raopTxt)
    }

    fun unregister() {
        listeners.forEach { runCatching { nsd.unregisterService(it) } }
        listeners.clear()
    }

    private fun publish(name: String, type: String, port: Int, txt: Map<String, String>) {
        val info = NsdServiceInfo().apply {
            serviceName = name
            serviceType = type
            setPort(port)
            txt.forEach { (k, v) ->
                runCatching { setAttribute(k, v) }.onFailure { Log.w(TAG, "TXT $k rejected: ${it.message}") }
            }
        }
        val listener = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(info: NsdServiceInfo) {
                Log.i(TAG, "registered ${info.serviceName} $type")
            }
            override fun onRegistrationFailed(info: NsdServiceInfo, code: Int) {
                Log.e(TAG, "register $type failed: $code")
            }
            override fun onServiceUnregistered(info: NsdServiceInfo) = Unit
            override fun onUnregistrationFailed(info: NsdServiceInfo, code: Int) = Unit
        }
        listeners += listener
        nsd.registerService(info, NsdManager.PROTOCOL_DNS_SD, listener)
    }

    private companion object { const val TAG = "AirPlayAdvertiser" }
}
