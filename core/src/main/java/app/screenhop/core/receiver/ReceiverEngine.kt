package app.screenhop.core.receiver

import android.view.Surface
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.emptyFlow

/** Protocols ScreenHop can receive. */
enum class Protocol { AIRPLAY, DLNA, MIRACAST_MICE }

/** What the sender is pushing to us. */
enum class SessionKind {
    /** Live screen mirroring (H.264/H.265 stream decoded to a Surface). */
    MIRROR,
    /** Sender handed us a media URL; we play it with ExoPlayer. */
    MEDIA_URL,
    /** Audio only (AirPlay audio / DLNA music). */
    AUDIO,
}

data class ReceiverConfig(
    /** Name shown on the sender's device list, e.g. "ScreenHop Living Room". */
    val deviceName: String,
    /** Stable per-install id; AirPlay uses it as a MAC-style device id. */
    val deviceId: String,
)

sealed interface EngineState {
    data object Stopped : EngineState
    /** Advertising on the LAN, waiting for a sender. */
    data object Advertising : EngineState
    data class Connected(
        val clientName: String,
        val kind: SessionKind,
        /** Set for [SessionKind.MEDIA_URL]. */
        val mediaUrl: String? = null,
        val startPositionMs: Long = 0,
        /** Mirrored picture size (0 = unknown); used to keep the aspect ratio. */
        val videoWidth: Int = 0,
        val videoHeight: Int = 0,
    ) : EngineState
    data class Error(val message: String) : EngineState
}

/** Media player state for [SessionKind.MEDIA_URL] sessions, reported back to the sender. */
data class PlaybackStatus(
    val durationMs: Long,
    val positionMs: Long,
    val playing: Boolean,
    val ready: Boolean,
    val buffering: Boolean,
    val ended: Boolean,
)

/** Remote control from the sender for [SessionKind.MEDIA_URL] sessions. */
sealed interface PlaybackCommand {
    data class Seek(val positionMs: Long) : PlaybackCommand
    data class SetPlaying(val playing: Boolean) : PlaybackCommand
}

/**
 * One protocol implementation (AirPlay, DLNA, MS-MICE...).
 * Engines advertise themselves, accept a single sender and report state.
 */
interface ReceiverEngine {
    val protocol: Protocol
    val state: StateFlow<EngineState>

    fun start(config: ReceiverConfig)
    fun stop()

    /** Kick the current sender (e.g. cast time ran out). Keeps advertising. */
    fun disconnectClient()

    /** Surface for mirrored video; null when the view goes away. Ignored by engines without video. */
    fun setVideoSurface(surface: Surface?) {}

    /** Player state for the current MEDIA_URL session (the sender polls it for its progress bar). */
    fun reportPlayback(status: PlaybackStatus) {}

    /** Seek / pause / resume requests from the sender. */
    val playbackCommands: Flow<PlaybackCommand> get() = emptyFlow()
}
