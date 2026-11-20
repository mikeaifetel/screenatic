package app.screenhop.dlna

import android.os.SystemClock
import android.util.Log
import java.net.InetSocketAddress
import java.net.Socket
import java.net.URL
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/**
 * GENA eventing: controllers SUBSCRIBE to a service's event URL and we NOTIFY them with LastChange
 * so their play/pause button and volume slider follow the TV. Position is polled, not evented.
 */
internal class Gena(
    /** Full current state for a service, sent as the initial event and after every change. */
    private val snapshot: (serviceType: String) -> String?,
) {
    private class Sub(val service: String, val callbacks: List<URL>, @Volatile var expiresAt: Long) {
        var seq = 0L
    }

    private val subs = ConcurrentHashMap<String, Sub>()
    private val sender = Executors.newSingleThreadScheduledExecutor { r -> Thread(r, "dlna-gena").apply { isDaemon = true } }

    fun handle(req: HttpRequest, serviceType: String): HttpResponse {
        val sid = req.header("sid")
        val timeoutSec = req.header("timeout")?.substringAfter("Second-", "")?.toLongOrNull()?.coerceIn(60, 3600) ?: 1800
        return when (req.method) {
            "SUBSCRIBE" -> if (sid != null) {
                val sub = subs[sid] ?: return HttpResponse.status(412)
                sub.expiresAt = SystemClock.elapsedRealtime() + timeoutSec * 1000
                ok(sid, timeoutSec)
            } else {
                val callbacks = CALLBACK_RE.findAll(req.header("callback").orEmpty())
                    .mapNotNull { runCatching { URL(it.groupValues[1]) }.getOrNull() }.toList()
                if (callbacks.isEmpty() || req.header("nt") != "upnp:event") return HttpResponse.status(412)
                val newSid = "uuid:${UUID.randomUUID()}"
                subs[newSid] = Sub(serviceType, callbacks, SystemClock.elapsedRealtime() + timeoutSec * 1000)
                // Initial event must follow the SUBSCRIBE response.
                sender.schedule({ snapshot(serviceType)?.let { notify(newSid, it) } }, 200, TimeUnit.MILLISECONDS)
                ok(newSid, timeoutSec)
            }
            "UNSUBSCRIBE" -> if (sid != null && subs.remove(sid) != null) HttpResponse.status(200) else HttpResponse.status(412)
            else -> HttpResponse.status(405)
        }
    }

    /** Push the current state of [serviceType] to its subscribers. */
    fun publish(serviceType: String) {
        sender.execute {
            val body = snapshot(serviceType) ?: return@execute
            val now = SystemClock.elapsedRealtime()
            subs.entries.removeIf { it.value.expiresAt < now }
            subs.filterValues { it.service == serviceType }.keys.forEach { notify(it, body) }
        }
    }

    fun clear() = subs.clear()

    private fun ok(sid: String, timeoutSec: Long) = HttpResponse(
        200, headers = listOf("SID" to sid, "TIMEOUT" to "Second-$timeoutSec", "DATE" to httpDate()),
    )

    /** NOTIFY isn't a method HttpURLConnection allows, so write the request by hand. */
    private fun notify(sid: String, body: String) {
        val sub = subs[sid] ?: return
        val bytes = body.toByteArray(Charsets.UTF_8)
        val seq = sub.seq++
        for (url in sub.callbacks) {
            val ok = runCatching {
                Socket().use { s ->
                    s.connect(InetSocketAddress(url.host, if (url.port > 0) url.port else 80), 3_000)
                    s.soTimeout = 3_000
                    val path = url.file.ifEmpty { "/" }
                    val head = "NOTIFY $path HTTP/1.1\r\nHOST: ${url.host}:${url.port}\r\n" +
                        "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\nNT: upnp:event\r\nNTS: upnp:propchange\r\n" +
                        "SID: $sid\r\nSEQ: $seq\r\nCONTENT-LENGTH: ${bytes.size}\r\nCONNECTION: close\r\n\r\n"
                    s.getOutputStream().apply { write(head.toByteArray(Charsets.ISO_8859_1)); write(bytes); flush() }
                    s.getInputStream().read()  // wait for the status line; content irrelevant
                }
            }.isSuccess
            if (ok) return
        }
        Log.d(TAG, "event delivery failed for $sid")
    }

    private fun httpDate(): String =
        java.text.SimpleDateFormat("EEE, dd MMM yyyy HH:mm:ss 'GMT'", java.util.Locale.US)
            .apply { timeZone = java.util.TimeZone.getTimeZone("GMT") }.format(java.util.Date())

    companion object {
        private const val TAG = "DlnaGena"
        private val CALLBACK_RE = Regex("<([^>]+)>")
    }
}
