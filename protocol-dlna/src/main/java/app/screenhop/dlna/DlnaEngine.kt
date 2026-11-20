package app.screenhop.dlna

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.os.SystemClock
import android.util.Log
import app.screenhop.core.receiver.EngineState
import app.screenhop.core.receiver.MediaType
import app.screenhop.core.receiver.NowPlaying
import app.screenhop.core.receiver.PlaybackCommand
import app.screenhop.core.receiver.PlaybackStatus
import app.screenhop.core.receiver.Protocol
import app.screenhop.core.receiver.ReceiverConfig
import app.screenhop.core.receiver.ReceiverEngine
import app.screenhop.core.receiver.SessionKind
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import java.io.ByteArrayOutputStream
import java.util.UUID
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit

/**
 * DLNA / UPnP MediaRenderer (DMR): Android phones' "cast to" (BubbleUPnP, gallery and video apps),
 * Windows "Cast to Device", NAS apps. The controller hands us a URL and drives play/pause/seek/volume;
 * playback itself is the app's ExoPlayer (MEDIA_URL session), images go to the image viewer.
 *
 * Hand-rolled SSDP + HTTP/SOAP + GENA (no UPnP library): small, no license questions.
 */
class DlnaEngine(private val context: Context) : ReceiverEngine {

    override val protocol = Protocol.DLNA

    private val _state = MutableStateFlow<EngineState>(EngineState.Stopped)
    override val state: StateFlow<EngineState> = _state.asStateFlow()

    private val commands = MutableSharedFlow<PlaybackCommand>(extraBufferCapacity = 16)
    override val playbackCommands: Flow<PlaybackCommand> = commands.asSharedFlow()

    private val prefs = context.getSharedPreferences("dlna", Context.MODE_PRIVATE)
    private val timer = Executors.newSingleThreadScheduledExecutor { r -> Thread(r, "dlna-timer").apply { isDaemon = true } }

    private var http: MiniHttpServer? = null
    private var ssdp: SsdpServer? = null
    private val gena = Gena(::eventSnapshot)
    private lateinit var config: ReceiverConfig
    private lateinit var uuid: String

    // ---- transport state (guarded by `lock`) ----
    private val lock = Any()
    private var uri: String? = null
    private var uriMeta = ""
    private var nextUri: String? = null
    private var nextMeta = ""
    private var transport = "NO_MEDIA_PRESENT"
    private var status: PlaybackStatus? = null
    private var mediaType = MediaType.VIDEO
    private var metaDurationMs = 0L
    private var controller = "DLNA"
    private var volume = prefs.getInt(PREF_VOLUME, 100)
    private var muted = false
    private var idleTimeout: ScheduledFuture<*>? = null
    /** After Play/Pause, player reports from before the command lands are ignored until this time. */
    private var commandAt = 0L
    private var wantPlaying = true

    override fun start(config: ReceiverConfig) {
        if (http != null) return
        this.config = config
        uuid = UUID.nameUUIDFromBytes("screenhop-dmr:${config.deviceId}".toByteArray()).toString()
        val server = MiniHttpServer(::route)
        http = server
        // Network work off the main thread; the port is fixed when free so controllers' caches stay valid.
        Thread({
            val port = server.start(PREFERRED_PORT)
            Log.i(TAG, "DLNA renderer on port $port, uuid $uuid")
            ssdp = SsdpServer(uuid) { findLanAddress()?.let { "http://${it.ip.hostAddress}:$port/description.xml" } }
                .also { it.start() }
        }, "dlna-start").start()
        _state.value = EngineState.Advertising
    }

    override fun stop() {
        ssdp?.stop()
        http?.stop()
        ssdp = null
        http = null
        gena.clear()
        synchronized(lock) { transport = if (uri == null) "NO_MEDIA_PRESENT" else "STOPPED" }
        _state.value = EngineState.Stopped
    }

    /** Cast time ran out, or a sender on another protocol took over. */
    override fun disconnectClient() {
        if (_state.value !is EngineState.Connected) return
        synchronized(lock) { transport = "STOPPED"; status = null }
        _state.value = EngineState.Advertising
        gena.publish(Upnp.AVT)
    }

    override fun reportPlayback(status: PlaybackStatus) {
        var ended = false
        val changed = synchronized(lock) {
            this.status = status
            // A report still showing the old play/pause state right after a command: don't flip-flop.
            if (SystemClock.elapsedRealtime() - commandAt < COMMAND_SETTLE_MS && !status.ended && !status.failed &&
                status.playing != wantPlaying
            ) return
            val t = when {
                status.failed -> "STOPPED"
                status.ended -> "STOPPED"
                status.playing && status.buffering -> "TRANSITIONING"
                status.playing -> "PLAYING"
                status.ready -> "PAUSED_PLAYBACK"
                else -> "TRANSITIONING"
            }
            ended = status.ended && transport != "STOPPED"
            (t != transport).also { transport = t }
        }
        if (changed) gena.publish(Upnp.AVT)
        if (ended) onEnded()
    }

    // ---------------------------------------------------------------- HTTP routing

    private fun route(req: HttpRequest): HttpResponse {
        val segments = req.path.trim('/').split('/')
        return when {
            req.path == "/description.xml" -> HttpResponse.xml(Upnp.deviceDescription(config.deviceName, uuid))
            req.path == "/icon.png" -> HttpResponse(200, icon, "image/png")
            segments.size == 2 && segments[1] == "scpd.xml" ->
                Scpd.forPath(segments[0])?.let { HttpResponse.xml(it) } ?: HttpResponse.status(404)
            segments.size == 2 && segments[1] == "control" && req.method == "POST" -> control(segments[0], req)
            segments.size == 2 && segments[1] == "event" -> {
                val type = Upnp.services.entries.firstOrNull { it.value == segments[0] }?.key
                    ?: return HttpResponse.status(404)
                gena.handle(req, type)
            }
            else -> HttpResponse.status(404)
        }
    }

    private class UpnpError(val code: Int, message: String) : Exception(message)

    private fun control(service: String, req: HttpRequest): HttpResponse {
        val type = Upnp.services.entries.firstOrNull { it.value == service }?.key ?: return HttpResponse.status(404)
        val call = parseSoap(req.bodyText) ?: return HttpResponse.xml(Upnp.soapFault(401, "Invalid Action"), 500)
        if (call.action !in POLLED) Log.d(TAG, "$service.${call.action} ${call.args.filterKeys { it != "CurrentURIMetaData" && it != "NextURIMetaData" }}")
        return try {
            val out = when (type) {
                Upnp.AVT -> avTransport(call, req)
                Upnp.RCS -> renderingControl(call)
                else -> connectionManager(call)
            }
            HttpResponse.xml(Upnp.soapResponse(type, call.action, out))
        } catch (e: UpnpError) {
            Log.d(TAG, "${call.action} -> ${e.code} ${e.message}")
            HttpResponse.xml(Upnp.soapFault(e.code, e.message ?: ""), 500)
        }
    }

    // ---------------------------------------------------------------- AVTransport

    private fun avTransport(call: SoapCall, req: HttpRequest): List<Pair<String, String>> {
        val a = call.args
        return when (call.action) {
            "SetAVTransportURI" -> {
                val newUri = a["CurrentURI"]?.trim().orEmpty()
                if (newUri.isEmpty()) throw UpnpError(402, "Invalid Args")
                val playingNow = _state.value is EngineState.Connected
                synchronized(lock) {
                    uri = newUri
                    uriMeta = a["CurrentURIMetaData"].orEmpty()
                    nextUri = null
                    controller = controllerName(req)
                    status = null
                    transport = "STOPPED"
                }
                // Already showing something: switch right away (what TVs do; many controllers send Play next anyway).
                if (playingNow) connect() else gena.publish(Upnp.AVT)
                emptyList()
            }
            "SetNextAVTransportURI" -> {
                synchronized(lock) {
                    nextUri = a["NextURI"]?.trim()?.ifEmpty { null }
                    nextMeta = a["NextURIMetaData"].orEmpty()
                }
                emptyList()
            }
            "Play" -> {
                val current = _state.value as? EngineState.Connected
                val u = synchronized(lock) { uri } ?: throw UpnpError(701, "Transition not available")
                if (current != null && current.mediaUrl == u) {
                    cancelIdleTimeout()
                    if (synchronized(lock) { status?.ended == true }) commands.tryEmit(PlaybackCommand.Seek(0))  // replay
                    markCommand(playing = true)
                    commands.tryEmit(PlaybackCommand.SetPlaying(true))
                    setTransport(if (mediaType == MediaType.IMAGE) "PLAYING" else "TRANSITIONING")
                } else {
                    connect()
                }
                emptyList()
            }
            "Pause" -> {
                if (_state.value !is EngineState.Connected) throw UpnpError(701, "Transition not available")
                markCommand(playing = false)
                commands.tryEmit(PlaybackCommand.SetPlaying(false))
                setTransport("PAUSED_PLAYBACK")
                emptyList()
            }
            "Stop" -> {
                cancelIdleTimeout()
                synchronized(lock) { status = null; transport = if (uri == null) "NO_MEDIA_PRESENT" else "STOPPED" }
                if (_state.value is EngineState.Connected) _state.value = EngineState.Advertising
                gena.publish(Upnp.AVT)
                emptyList()
            }
            "Seek" -> {
                val unit = a["Unit"].orEmpty()
                if (unit == "REL_TIME" || unit == "ABS_TIME") {
                    val ms = Upnp.parseTime(a["Target"].orEmpty()) ?: throw UpnpError(711, "Illegal seek target")
                    commands.tryEmit(PlaybackCommand.Seek(ms))
                }
                emptyList()
            }
            "Next", "Previous" -> emptyList()  // single-item queue: the controller manages its playlist
            "GetTransportInfo" -> synchronized(lock) {
                listOf("CurrentTransportState" to transport, "CurrentTransportStatus" to transportStatus(), "CurrentSpeed" to "1")
            }
            "GetPositionInfo" -> synchronized(lock) {
                val s = status
                val dur = s?.durationMs?.takeIf { it > 0 } ?: metaDurationMs
                val pos = s?.positionMs ?: 0
                listOf(
                    "Track" to if (uri != null) "1" else "0",
                    "TrackDuration" to Upnp.formatTime(dur),
                    "TrackMetaData" to uriMeta,
                    "TrackURI" to uri.orEmpty(),
                    "RelTime" to Upnp.formatTime(pos),
                    "AbsTime" to Upnp.formatTime(pos),
                    "RelCount" to "2147483647",
                    "AbsCount" to "2147483647",
                )
            }
            "GetMediaInfo" -> synchronized(lock) {
                val dur = status?.durationMs?.takeIf { it > 0 } ?: metaDurationMs
                listOf(
                    "NrTracks" to if (uri != null) "1" else "0",
                    "MediaDuration" to Upnp.formatTime(dur),
                    "CurrentURI" to uri.orEmpty(),
                    "CurrentURIMetaData" to uriMeta,
                    "NextURI" to nextUri.orEmpty(),
                    "NextURIMetaData" to nextMeta,
                    "PlayMedium" to if (uri != null) "NETWORK" else "NONE",
                    "RecordMedium" to "NOT_IMPLEMENTED",
                    "WriteStatus" to "NOT_IMPLEMENTED",
                )
            }
            "GetDeviceCapabilities" -> listOf(
                "PlayMedia" to "NETWORK", "RecMedia" to "NOT_IMPLEMENTED", "RecQualityModes" to "NOT_IMPLEMENTED",
            )
            "GetTransportSettings" -> listOf("PlayMode" to "NORMAL", "RecQualityMode" to "NOT_IMPLEMENTED")
            "GetCurrentTransportActions" -> listOf("Actions" to transportActions())
            else -> throw UpnpError(401, "Invalid Action")
        }
    }

    /** Show the current URI: a new MEDIA_URL session (the player/viewer is keyed by URL). */
    private fun connect() {
        cancelIdleTimeout()
        val (u, didl, name) = synchronized(lock) { Triple(uri ?: return, parseDidl(uriMeta), controller) }
        val type = didl.mediaType ?: guessMediaType(u)
        markCommand(playing = true)
        synchronized(lock) {
            mediaType = type
            metaDurationMs = didl.durationMs
            status = null
            transport = if (type == MediaType.IMAGE) "PLAYING" else "TRANSITIONING"
        }
        Log.i(TAG, "play $type from $name: ${didl.title.ifEmpty { u }}")
        // Same URL already on screen: the player is keyed by URL and would just carry on, so restart it.
        if ((_state.value as? EngineState.Connected)?.mediaUrl == u) {
            commands.tryEmit(PlaybackCommand.Seek(0))
            commands.tryEmit(PlaybackCommand.SetPlaying(true))
        }
        _state.value = EngineState.Connected(
            clientName = name,
            kind = SessionKind.MEDIA_URL,
            mediaUrl = u,
            nowPlaying = NowPlaying(
                title = didl.title, artist = didl.artist, album = didl.album, artworkUrl = didl.artworkUrl,
                durationMs = didl.durationMs,
            ),
            mediaType = type,
            playerVolume = playerVolume(),
        )
        gena.publish(Upnp.AVT)
    }

    /** Track finished: play the queued next URI, else go back home after a moment (controllers queue the next item then). */
    private fun onEnded() {
        val hasNext = synchronized(lock) {
            val n = nextUri ?: return@synchronized false
            uri = n; uriMeta = nextMeta; nextUri = null; nextMeta = ""
            true
        }
        if (hasNext) {
            connect()
            return
        }
        cancelIdleTimeout()
        idleTimeout = timer.schedule({
            val stillEnded = synchronized(lock) { status?.ended == true }
            if (stillEnded && _state.value is EngineState.Connected) _state.value = EngineState.Advertising
        }, IDLE_AFTER_END_SEC, TimeUnit.SECONDS)
    }

    private fun markCommand(playing: Boolean) = synchronized(lock) {
        wantPlaying = playing
        commandAt = SystemClock.elapsedRealtime()
    }

    private fun cancelIdleTimeout() {
        idleTimeout?.cancel(false)
        idleTimeout = null
    }

    private fun setTransport(t: String) {
        val changed = synchronized(lock) { (transport != t).also { transport = t } }
        if (changed) gena.publish(Upnp.AVT)
    }

    private fun transportActions(): String = synchronized(lock) { transportActionsLocked() }

    // ---------------------------------------------------------------- RenderingControl / ConnectionManager

    private fun renderingControl(call: SoapCall): List<Pair<String, String>> = when (call.action) {
        "GetVolume" -> listOf("CurrentVolume" to synchronized(lock) { volume }.toString())
        "SetVolume" -> {
            val v = call.args["DesiredVolume"]?.toIntOrNull()?.takeIf { it in 0..100 } ?: throw UpnpError(402, "Invalid Args")
            synchronized(lock) { volume = v; if (v > 0) muted = false }
            prefs.edit().putInt(PREF_VOLUME, v).apply()
            applyVolume()
            emptyList()
        }
        "GetMute" -> listOf("CurrentMute" to if (synchronized(lock) { muted }) "1" else "0")
        "SetMute" -> {
            val m = call.args["DesiredMute"].orEmpty().let { it == "1" || it.equals("true", true) || it.equals("yes", true) }
            synchronized(lock) { muted = m }
            applyVolume()
            emptyList()
        }
        "ListPresets" -> listOf("CurrentPresetNameList" to "FactoryDefaults")
        "SelectPreset" -> emptyList()
        else -> throw UpnpError(401, "Invalid Action")
    }

    private fun applyVolume() {
        val v = playerVolume()
        _state.update { s -> if (s is EngineState.Connected) s.copy(playerVolume = v) else s }
        gena.publish(Upnp.RCS)
    }

    /** UPnP 0..100 -> player gain with a perceptual curve (linear slider positions feel too loud at the top). */
    private fun playerVolume(): Float = synchronized(lock) {
        if (muted) 0f else (volume / 100f).let { it * it }
    }

    private fun connectionManager(call: SoapCall): List<Pair<String, String>> = when (call.action) {
        "GetProtocolInfo" -> listOf("Source" to "", "Sink" to Upnp.sinkProtocolInfo)
        "GetCurrentConnectionIDs" -> listOf("ConnectionIDs" to "0")
        "GetCurrentConnectionInfo" -> listOf(
            "RcsID" to "0", "AVTransportID" to "0", "ProtocolInfo" to "", "PeerConnectionManager" to "",
            "PeerConnectionID" to "-1", "Direction" to "Input", "Status" to "OK",
        )
        else -> throw UpnpError(401, "Invalid Action")
    }

    // ---------------------------------------------------------------- events

    private fun eventSnapshot(serviceType: String): String? = when (serviceType) {
        Upnp.AVT -> synchronized(lock) {
            val dur = Upnp.formatTime(status?.durationMs?.takeIf { it > 0 } ?: metaDurationMs)
            Upnp.lastChangeEvent(
                Upnp.AVT,
                listOf(
                    "TransportState" to transport,
                    "TransportStatus" to transportStatus(),
                    "CurrentTransportActions" to transportActionsLocked(),
                    "AVTransportURI" to uri.orEmpty(),
                    "CurrentTrackURI" to uri.orEmpty(),
                    "NumberOfTracks" to if (uri != null) "1" else "0",
                    "CurrentTrack" to if (uri != null) "1" else "0",
                    "CurrentTrackDuration" to dur,
                    "CurrentMediaDuration" to dur,
                    "CurrentPlayMode" to "NORMAL",
                    "TransportPlaySpeed" to "1",
                ),
            )
        }
        Upnp.RCS -> synchronized(lock) {
            Upnp.lastChangeEvent(Upnp.RCS, listOf("Volume" to volume.toString(), "Mute" to if (muted) "1" else "0"))
        }
        else -> null  // ConnectionManager: nothing changes
    }

    private fun transportStatus(): String = if (status?.failed == true) "ERROR_OCCURRED" else "OK"

    private fun transportActionsLocked(): String = when (transport) {
        "PLAYING", "TRANSITIONING" -> "Pause,Stop,Seek"
        "PAUSED_PLAYBACK" -> "Play,Stop,Seek"
        "STOPPED" -> "Play"
        else -> ""
    }

    // ---------------------------------------------------------------- helpers

    /** Shown on the TV ("Playing from ..."): the controller's product name, else its address. */
    private fun controllerName(req: HttpRequest): String {
        val ua = req.header("user-agent").orEmpty()
        return when {
            ua.contains("Windows", ignoreCase = true) -> "Windows PC"
            ua.contains("BubbleUPnP", ignoreCase = true) -> "BubbleUPnP"
            ua.contains("Android", ignoreCase = true) -> "Android"
            else -> req.remote.inetAddress?.hostAddress ?: "DLNA"
        }
    }

    private val icon: ByteArray by lazy {
        runCatching {
            val d = context.packageManager.getApplicationIcon(context.packageName)
            val bmp = Bitmap.createBitmap(192, 192, Bitmap.Config.ARGB_8888)
            d.setBounds(0, 0, 192, 192)
            d.draw(Canvas(bmp))
            ByteArrayOutputStream().also { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) }.toByteArray()
        }.getOrDefault(ByteArray(0))
    }

    companion object {
        private const val TAG = "DlnaEngine"
        private const val PREFERRED_PORT = 49595
        private const val PREF_VOLUME = "volume"
        private const val IDLE_AFTER_END_SEC = 8L
        private const val COMMAND_SETTLE_MS = 1500L
        /** Polled every second by controllers; not worth a log line. */
        private val POLLED = setOf("GetPositionInfo", "GetTransportInfo", "GetVolume", "GetMute", "GetMediaInfo")
    }
}
