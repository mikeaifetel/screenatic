package com.screenatic.dlna

import android.util.Log
import java.io.BufferedInputStream
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketException
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

internal class HttpRequest(
    val method: String,
    val path: String,
    /** Lower-cased header names. */
    val headers: Map<String, String>,
    val body: ByteArray,
    val remote: Socket,
) {
    fun header(name: String): String? = headers[name.lowercase()]
    val bodyText: String get() = String(body, Charsets.UTF_8)
}

internal class HttpResponse(
    val status: Int,
    val body: ByteArray = ByteArray(0),
    val contentType: String? = null,
    val headers: List<Pair<String, String>> = emptyList(),
) {
    companion object {
        fun xml(text: String, status: Int = 200) = HttpResponse(status, text.toByteArray(Charsets.UTF_8), "text/xml; charset=\"utf-8\"")
        fun status(code: Int) = HttpResponse(code)
    }
}

/**
 * Just enough HTTP/1.1 for UPnP: GET, POST (SOAP), SUBSCRIBE/UNSUBSCRIBE (GENA). One request per
 * connection (we always answer Connection: close), a thread per connection.
 */
internal class MiniHttpServer(private val handler: (HttpRequest) -> HttpResponse) {
    private var server: ServerSocket? = null
    private var pool: ExecutorService? = null
    val port: Int get() = server?.localPort ?: 0

    fun start(preferredPort: Int): Int {
        val socket = ServerSocket().apply { reuseAddress = true }
        try {
            socket.bind(InetSocketAddress(preferredPort))
        } catch (e: SocketException) {
            socket.bind(InetSocketAddress(0))  // port taken: any port works, LOCATION carries it
        }
        server = socket
        val workers = Executors.newCachedThreadPool { r -> Thread(r, "dlna-http").apply { isDaemon = true } }
        pool = workers
        Thread({
            while (!socket.isClosed) {
                val client = try { socket.accept() } catch (e: Exception) { break }
                workers.execute { serve(client) }
            }
        }, "dlna-accept").apply { isDaemon = true }.start()
        return socket.localPort
    }

    fun stop() {
        runCatching { server?.close() }
        pool?.shutdownNow()
        server = null
        pool = null
    }

    private fun serve(client: Socket): Unit = client.use { s ->
        try {
            s.soTimeout = 10_000
            val input = BufferedInputStream(s.getInputStream())
            val requestLine = readLine(input) ?: return
            val parts = requestLine.split(' ')
            if (parts.size < 2) return
            val headers = HashMap<String, String>()
            while (true) {
                val line = readLine(input) ?: break
                if (line.isEmpty()) break
                val i = line.indexOf(':')
                if (i > 0) headers[line.substring(0, i).trim().lowercase()] = line.substring(i + 1).trim()
            }
            val body = readBody(input, headers)
            val req = HttpRequest(parts[0].uppercase(), parts[1].substringBefore('?'), headers, body, s)
            val res = runCatching { handler(req) }.getOrElse {
                Log.w(TAG, "handler failed for ${req.method} ${req.path}", it)
                HttpResponse.status(500)
            }
            write(s, res, includeBody = req.method != "HEAD")
        } catch (e: Exception) {
            Log.d(TAG, "connection error: ${e.message}")
        }
    }

    private fun readBody(input: InputStream, headers: Map<String, String>): ByteArray {
        headers["content-length"]?.toIntOrNull()?.let { len ->
            if (len <= 0) return ByteArray(0)
            val buf = ByteArray(minOf(len, MAX_BODY))
            var off = 0
            while (off < buf.size) {
                val n = input.read(buf, off, buf.size - off)
                if (n < 0) break
                off += n
            }
            return if (off == buf.size) buf else buf.copyOf(off)
        }
        if (headers["transfer-encoding"]?.contains("chunked", ignoreCase = true) == true) {
            val out = ByteArrayOutputStream()
            while (out.size() < MAX_BODY) {
                val size = readLine(input)?.substringBefore(';')?.trim()?.toIntOrNull(16) ?: break
                if (size == 0) break
                val chunk = ByteArray(size)
                var off = 0
                while (off < size) {
                    val n = input.read(chunk, off, size - off)
                    if (n < 0) break
                    off += n
                }
                out.write(chunk, 0, off)
                readLine(input)  // CRLF after the chunk
            }
            return out.toByteArray()
        }
        return ByteArray(0)
    }

    private fun write(s: Socket, res: HttpResponse, includeBody: Boolean) {
        val sb = StringBuilder()
        sb.append("HTTP/1.1 ${res.status} ${reason(res.status)}\r\n")
        sb.append("SERVER: $SERVER_HEADER\r\n")
        sb.append("Connection: close\r\n")
        sb.append("Content-Length: ${res.body.size}\r\n")
        res.contentType?.let { sb.append("Content-Type: $it\r\n") }
        for ((k, v) in res.headers) sb.append("$k: $v\r\n")
        sb.append("\r\n")
        val out = s.getOutputStream()
        out.write(sb.toString().toByteArray(Charsets.ISO_8859_1))
        if (includeBody) out.write(res.body)
        out.flush()
    }

    private fun readLine(input: InputStream): String? {
        val sb = StringBuilder()
        while (true) {
            val c = input.read()
            if (c < 0) return if (sb.isEmpty()) null else sb.toString()
            if (c == '\n'.code) return sb.toString().trimEnd('\r')
            if (sb.length > 8192) return null
            sb.append(c.toChar())
        }
    }

    private fun reason(code: Int) = when (code) {
        200 -> "OK"; 400 -> "Bad Request"; 404 -> "Not Found"; 405 -> "Method Not Allowed"
        412 -> "Precondition Failed"; 500 -> "Internal Server Error"; else -> "Status"
    }

    companion object {
        private const val TAG = "DlnaHttp"
        private const val MAX_BODY = 1 shl 20
    }
}
