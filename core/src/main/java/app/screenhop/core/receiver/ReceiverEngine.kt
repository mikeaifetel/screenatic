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

/** What a [SessionKind.MEDIA_URL] points at; decides player vs. image viewer and the audio overlay. */
enum class MediaType { VIDEO, AUDIO, IMAGE }

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
        /** Track info for audio sessions (AirPlay from Music, Podcasts, ...). */
        val nowPlaying: NowPlaying? = null,
        /** For [SessionKind.MEDIA_URL]: what the URL is (senders that don't say get VIDEO). */
        val mediaType: MediaType = MediaType.VIDEO,
        /** Player volume 0..1 set by the sender (DLNA SetVolume); AirPlay URL playback keeps 1. */
        val playerVolume: Float = 1f,
    ) : EngineState
    data class Error(val message: String) : EngineState
}

/** What an audio sender is playing. Position is a snapshot taken at [progressAtMs] (elapsedRealtime). */
data class NowPlaying(
    val title: String = "",
    val artist: String = "",
    val album: String = "",
    /** Cover art as sent (JPEG/PNG); compared by reference, replaced on every new image. */
    val artwork: ByteArray? = null,
    /** Cover art by URL (DLNA albumArtURI); used when [artwork] is null. */
    val artworkUrl: String? = null,
    val positionMs: Long = 0,
    val durationMs: Long = 0,
    val progressAtMs: Long = 0,
    /** False while the sender is paused; the position then stays at [positionMs]. */
    val playing: Boolean = true,
) {
    /** Position at [nowMs] (elapsedRealtime), advancing only while playing. */
    fun positionAt(nowMs: Long): Long {
        val p = if (playing) positionMs + (nowMs - progressAtMs) else positionMs
        return if (durationMs > 0) p.coerceIn(0, durationMs) else p.coerceAtLeast(0)
    }
}

/** Media player state for [SessionKind.MEDIA_URL] sessions, reported back to the sender. */
data class PlaybackStatus(
    val durationMs: Long,
    val positionMs: Long,
    val playing: Boolean,
    val ready: Boolean,
    val buffering: Boolean,
    val ended: Boolean,
    /** The player gave up (HTTP error, unsupported format...). */
    val failed: Boolean = false,
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
