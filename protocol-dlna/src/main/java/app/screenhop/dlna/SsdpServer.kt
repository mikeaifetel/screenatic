package app.screenhop.dlna

import android.os.Build
import android.util.Log
import java.net.DatagramPacket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.MulticastSocket
import java.net.NetworkInterface
import java.net.SocketAddress
import kotlin.random.Random

internal val SERVER_HEADER = "Android/${Build.VERSION.RELEASE} UPnP/1.0 ScreenHop/1.0"

/** The LAN interface we serve on and its IPv4 address (Wi-Fi or Ethernet). */
internal data class LanAddress(val iface: NetworkInterface, val ip: Inet4Address)

internal fun findLanAddress(): LanAddress? = runCatching {
    NetworkInterface.getNetworkInterfaces().toList()
        .filter { it.isUp && !it.isLoopback && !it.isVirtual && it.supportsMulticast() }
        .sortedBy { if (it.name.startsWith("wlan") || it.name.startsWith("eth")) 0 else 1 }
        .firstNotNullOfOrNull { nif ->
            nif.inetAddresses.toList().filterIsInstance<Inet4Address>()
                .firstOrNull { it.isSiteLocalAddress }?.let { LanAddress(nif, it) }
        }
}.getOrNull()

/**
 * SSDP for a UPnP root device: answers M-SEARCH and sends ssdp:alive / ssdp:byebye on
 * 239.255.255.250:1900. Needs the Wi-Fi multicast lock (held by ReceiverService).
 */
internal class SsdpServer(
    private val uuid: String,
    /** Description URL, e.g. http://192.168.0.181:49595/description.xml; null while offline. */
    private val location: () -> String?,
) {
    private val group: InetAddress = InetAddress.getByName("239.255.255.250")
    private val groupAddr = InetSocketAddress(group, 1900)
    @Volatile private var socket: MulticastSocket? = null
    @Volatile private var running = false

    private val notificationTypes = listOf("upnp:rootdevice", "uuid:$uuid", Upnp.DEVICE_TYPE) + Upnp.services.keys

    private fun usn(nt: String) = if (nt == "uuid:$uuid") nt else "uuid:$uuid::$nt"

    fun start() {
        running = true
        Thread(::receiveLoop, "dlna-ssdp").apply { isDaemon = true }.start()
        Thread(::announceLoop, "dlna-ssdp-notify").apply { isDaemon = true }.start()
    }

    fun stop() {
        running = false
        val s = socket ?: return
        runCatching { repeat(2) { notificationTypes.forEach { send(s, byebye(it), groupAddr) } } }
        runCatching { s.close() }
        socket = null
    }

    private fun open(): MulticastSocket? {
        val lan = findLanAddress() ?: return null
        return runCatching {
            MulticastSocket(null).apply {
                reuseAddress = true
                bind(InetSocketAddress(1900))
                timeToLive = 4
                networkInterface = lan.iface
                joinGroup(groupAddr, lan.iface)
            }
        }.onFailure { Log.w(TAG, "SSDP socket failed", it) }.getOrNull()
    }

    private fun receiveLoop() {
        val buf = ByteArray(2048)
        while (running) {
            val s = socket ?: open()?.also { socket = it } ?: run { Thread.sleep(5_000); null } ?: continue
            try {
                val packet = DatagramPacket(buf, buf.size)
                s.receive(packet)
                val text = String(packet.data, 0, packet.length, Charsets.UTF_8)
                if (text.startsWith("M-SEARCH", ignoreCase = true)) onSearch(s, text, packet.socketAddress)
            } catch (e: Exception) {
                if (!running) break
                Log.d(TAG, "SSDP receive: ${e.message}; reopening")
                runCatching { s.close() }
                socket = null
                Thread.sleep(2_000)
            }
        }
    }

    private fun onSearch(s: MulticastSocket, text: String, from: SocketAddress) {
        val headers = text.lineSequence().drop(1).mapNotNull { line ->
            val i = line.indexOf(':')
            if (i > 0) line.substring(0, i).trim().uppercase() to line.substring(i + 1).trim() else null
        }.toMap()
        if (headers["MAN"]?.contains("ssdp:discover") != true) return
        val st = headers["ST"] ?: return
        val targets = when {
            st == "ssdp:all" -> notificationTypes
            st in notificationTypes -> listOf(st)
            else -> return
        }
        val mx = headers["MX"]?.toIntOrNull()?.coerceIn(1, 5) ?: 1
        Thread {
            // Spread replies over MX (capped at 1 s) as the spec asks; keeps phones' discovery snappy.
            Thread.sleep(Random.nextLong(0, minOf(mx, 1) * 1000L))
            targets.forEach { runCatching { send(s, searchResponse(it), from) } }
        }.start()
    }

    private fun announceLoop() {
        var round = 0
        while (running) {
            socket?.let { s -> notificationTypes.forEach { runCatching { send(s, alive(it), groupAddr) } } }
            // Burst at start (UDP is lossy), then every 30 s (well inside max-age 1800).
            Thread.sleep(if (round++ < 3) 1_000 else 30_000)
        }
    }

    private fun send(s: MulticastSocket, msg: String?, to: SocketAddress) {
        msg ?: return
        val bytes = msg.toByteArray(Charsets.UTF_8)
        s.send(DatagramPacket(bytes, bytes.size, to))
    }

    private fun alive(nt: String): String? {
        val loc = location() ?: return null
        return "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nCACHE-CONTROL: max-age=1800\r\n" +
            "LOCATION: $loc\r\nNT: $nt\r\nNTS: ssdp:alive\r\nSERVER: $SERVER_HEADER\r\nUSN: ${usn(nt)}\r\n\r\n"
    }

    private fun byebye(nt: String): String =
        "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nNT: $nt\r\nNTS: ssdp:byebye\r\nUSN: ${usn(nt)}\r\n\r\n"

    private fun searchResponse(st: String): String? {
        val loc = location() ?: return null
        return "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nEXT:\r\nLOCATION: $loc\r\n" +
            "SERVER: $SERVER_HEADER\r\nST: $st\r\nUSN: ${usn(st)}\r\n\r\n"
    }

    companion object { private const val TAG = "DlnaSsdp" }
}
