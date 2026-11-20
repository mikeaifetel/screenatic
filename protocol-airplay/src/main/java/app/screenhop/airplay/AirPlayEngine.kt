package app.screenhop.airplay

import android.content.Context
import android.media.MediaCodecList
import android.util.Log
import android.view.Surface
import app.screenhop.core.receiver.EngineState
import app.screenhop.core.receiver.Protocol
import app.screenhop.core.receiver.ReceiverConfig
import app.screenhop.core.receiver.ReceiverEngine
import app.screenhop.core.receiver.SessionKind
import app.screenhop.core.receiver.PlaybackCommand
import app.screenhop.core.receiver.PlaybackStatus
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import java.io.File

/**
 * AirPlay receiver. Protocol work (RTSP, pairing, FairPlay, mirroring, audio)
 * happens in libscreenhop_airplay.so, built on UxPlay's lib/ (GPL-3.0).
 * Video is decoded natively with AMediaCodec straight onto the Surface.
 */
class AirPlayEngine(context: Context) : ReceiverEngine {

    override val protocol = Protocol.AIRPLAY

    private val _state = MutableStateFlow<EngineState>(EngineState.Stopped)
    override val state: StateFlow<EngineState> = _state.asStateFlow()

    private val advertiser = AirPlayAdvertiser(context)
    /** Persistent ed25519 identity; keeping it means iPhones don't re-pair after a restart. */
    private val keyFile = File(context.noBackupFilesDir, "airplay_ed25519.pem")
    private var nativeHandle = 0L
    /** Last Surface from the UI; it can arrive before the native server exists. */
    @Volatile private var surface: Surface? = null

    override fun start(config: ReceiverConfig) {
        if (nativeHandle != 0L) return
        if (!nativeLoaded) {
            _state.value = EngineState.Error("native library missing")
            return
        }
        nativeHandle = nativeCreate(config.deviceName, config.deviceId, keyFile.absolutePath, hasHevcDecoder())
        surface?.let { nativeSetSurface(nativeHandle, it) }
        val ports = nativeStart(nativeHandle) // [airplayPort, raopPort] (same port today)
        if (ports == null || ports.size < 2) {
            nativeDestroy(nativeHandle)
            nativeHandle = 0L
            _state.value = EngineState.Error("AirPlay server failed to start")
            return
        }
        val airplayTxt = nativeTxtRecord(nativeHandle, TXT_AIRPLAY).toMap()
        val raopTxt = nativeTxtRecord(nativeHandle, TXT_RAOP).toMap()
        Log.d(TAG, "airplay TXT: $airplayTxt")
        Log.d(TAG, "raop TXT: $raopTxt")
        advertiser.register(
            deviceName = config.deviceName,
            deviceId = config.deviceId,
            airplayPort = ports[0],
            raopPort = ports[1],
            airplayTxt = airplayTxt,
            raopTxt = raopTxt,
        )
        _state.value = EngineState.Advertising
    }

    override fun stop() {
        advertiser.unregister()
        if (nativeHandle != 0L) {
            nativeStop(nativeHandle)
            nativeDestroy(nativeHandle)
            nativeHandle = 0L
        }
        _state.value = EngineState.Stopped
    }

    override fun disconnectClient() {
        if (nativeHandle != 0L) nativeDisconnectClients(nativeHandle)
    }

    private val commands = MutableSharedFlow<PlaybackCommand>(extraBufferCapacity = 16)
    override val playbackCommands: Flow<PlaybackCommand> = commands.asSharedFlow()

    override fun reportPlayback(status: PlaybackStatus) {
        if (nativeHandle == 0L) return
        nativeSetPlayback(
            nativeHandle,
            status.durationMs / 1000.0,
            status.positionMs / 1000.0,
            if (status.playing) 1f else 0f,
            status.ready,
            status.buffering,
            status.ended,
        )
    }

    override fun setVideoSurface(surface: Surface?) {
        this.surface = surface
        if (nativeHandle != 0L) nativeSetSurface(nativeHandle, surface)
    }

    // ---- Called from native code on protocol threads (keep in sync with jni_bridge.cpp) ----

    @Suppress("unused")
    private fun onClientConnected(clientName: String, kind: Int) {
        Log.i(TAG, "connected: $clientName kind=$kind")
        _state.value = EngineState.Connected(clientName, kindFromNative(kind))
    }

    @Suppress("unused")
    private fun onPlayUrl(clientName: String, url: String, startPositionMs: Long) {
        _state.value = EngineState.Connected(clientName, SessionKind.MEDIA_URL, url, startPositionMs)
    }

    @Suppress("unused")
    private fun onPlaybackCommand(command: Int, value: Double) {
        Log.i(TAG, "playback command $command $value")
        when (command) {
            CMD_SEEK -> commands.tryEmit(PlaybackCommand.Seek((value * 1000).toLong()))
            CMD_RATE -> commands.tryEmit(PlaybackCommand.SetPlaying(value > 0))
        }
    }

    @Suppress("unused")
    private fun onVideoSize(width: Int, height: Int) {
        _state.update { s ->
            if (s is EngineState.Connected) s.copy(videoWidth = width, videoHeight = height) else s
        }
    }

    @Suppress("unused")
    private fun onClientDisconnected() {
        Log.i(TAG, "disconnected")
        if (nativeHandle != 0L) _state.value = EngineState.Advertising
    }

    @Suppress("unused")
    private fun onNativeLog(message: String) {
        Log.d(TAG, message)
    }

    private fun kindFromNative(kind: Int) = when (kind) {
        KIND_AUDIO -> SessionKind.AUDIO
        else -> SessionKind.MIRROR
    }

    private fun Array<String>.toMap(): Map<String, String> =
        associate { it.substringBefore('=') to it.substringAfter('=', "") }

    private fun hasHevcDecoder(): Boolean = runCatching {
        MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.any { info ->
            !info.isEncoder && info.supportedTypes.any { it.equals("video/hevc", ignoreCase = true) }
        }
    }.getOrDefault(false)

    private external fun nativeCreate(deviceName: String, deviceId: String, keyFile: String, h265: Boolean): Long
    private external fun nativeStart(handle: Long): IntArray?
    private external fun nativeStop(handle: Long)
    private external fun nativeDestroy(handle: Long)
    private external fun nativeDisconnectClients(handle: Long)
    private external fun nativeSetSurface(handle: Long, surface: Surface?)
    private external fun nativeTxtRecord(handle: Long, which: Int): Array<String>
    private external fun nativeSetPlayback(
        handle: Long, durationSec: Double, positionSec: Double, rate: Float,
        ready: Boolean, buffering: Boolean, finished: Boolean,
    )

    companion object {
        private const val TAG = "AirPlayEngine"
        private const val TXT_AIRPLAY = 0
        private const val TXT_RAOP = 1
        private const val KIND_MIRROR = 0
        private const val KIND_AUDIO = 1
        private const val CMD_SEEK = 0
        private const val CMD_RATE = 1

        private val nativeLoaded = runCatching { System.loadLibrary("screenhop_airplay") }
            .onFailure { Log.e(TAG, "loadLibrary failed", it) }
            .isSuccess
    }
}
