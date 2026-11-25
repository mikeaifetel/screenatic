package com.screenatic.receiver.ui

import android.view.Surface
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.unit.IntOffset
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.requiredSize
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.remember
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.res.stringResource
import androidx.tv.material3.Text
import com.screenatic.receiver.R
import kotlinx.coroutines.delay
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import android.os.SystemClock
import android.util.Log
import com.screenatic.core.receiver.NowPlaying
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.PlaybackException
import androidx.media3.common.Player
import com.screenatic.core.receiver.PlaybackCommand
import com.screenatic.core.receiver.PlaybackStatus
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.isActive
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.ui.PlayerView
import com.screenatic.core.session.ReceiverUiState

/**
 * Live mirroring surface. Always composed (under the home screen when idle) so the native
 * decoder has a Surface the moment a sender starts streaming — frames that arrive before a
 * Surface exists cannot be shown without breaking the H.264 reference chain.
 */
@Composable
fun MirrorSurface(ui: ReceiverUiState, fill: Boolean, onSurface: (Surface?) -> Unit) {
    val w = ui.active?.videoWidth ?: 0
    val h = ui.active?.videoHeight ?: 0
    BoxWithConstraints(
        Modifier.fillMaxSize().background(Color.Black).clipToBounds(),
        contentAlignment = Alignment.Center,
    ) {
        val videoModifier = if (w > 0 && h > 0) {
            // Fit: whole picture visible (letter/pillarbox). Fill: cover the screen, crop the
            // overflow — a 19.5:9 phone playing 16:9 video then loses exactly its own side bars.
            val videoAspect = w.toFloat() / h
            val screenAspect = maxWidth / maxHeight
            val matchWidth = (videoAspect > screenAspect) != fill
            if (matchWidth) Modifier.requiredSize(maxWidth, maxWidth / videoAspect)
            else Modifier.requiredSize(maxHeight * videoAspect, maxHeight)
        } else {
            Modifier.fillMaxSize()
        }
        AndroidView(
            modifier = videoModifier,
            factory = { ctx ->
                SurfaceView(ctx).apply {
                    holder.addCallback(object : SurfaceHolder.Callback {
                        override fun surfaceCreated(holder: SurfaceHolder) = onSurface(holder.surface)
                        override fun surfaceChanged(holder: SurfaceHolder, f: Int, w: Int, h: Int) = Unit
                        override fun surfaceDestroyed(holder: SurfaceHolder) = onSurface(null)
                    })
                }
            },
        )
    }
}

/** Short "Fit / Fill — press OK" hint, shown for a few seconds after each [tick] change. */
@Composable
fun ZoomHint(fill: Boolean, tick: Int, modifier: Modifier = Modifier) {
    var visible by remember { mutableStateOf(false) }
    LaunchedEffect(tick) {
        if (tick == 0) return@LaunchedEffect
        visible = true
        delay(3_000)
        visible = false
    }
    if (!visible) return
    Text(
        text = stringResource(if (fill) R.string.zoom_fill else R.string.zoom_fit),
        color = Color.White,
        modifier = modifier
            .background(Color(0xB3000000), RoundedCornerShape(8.dp))
            .padding(horizontal = 20.dp, vertical = 10.dp),
    )
}

/**
 * AirPlay video (HLS from UxPlay's local playlist server, or Safari's own URL in fullscreen) and
 * DLNA SetAVTransportURI. Reports player state for the sender's progress bar and follows its
 * seek / pause / resume.
 *
 * Until the video shows its first frame (2.5 s at most) the player's view sits off screen and
 * [onShown] stays false, so the caller keeps the mirror's last frame on screen under a spinner
 * instead of a black flash. Same rules as Vega's UrlPlayer, found by testing there.
 */
@Composable
fun MediaUrlPlayer(
    url: String,
    startPositionMs: Long,
    ui: ReceiverUiState,
    commands: Flow<PlaybackCommand>,
    onStatus: (PlaybackStatus) -> Unit,
    onWatchAd: () -> Unit,
    /** Sender-controlled volume (DLNA SetVolume, AirPlay volume). */
    volume: Float = 1f,
    /** Set for audio-only media: show track info instead of a black video area. */
    audioInfo: NowPlaying? = null,
    clientName: String = "",
    /** True once the video is on screen: the mirror surface underneath can be released. */
    onShown: (Boolean) -> Unit = {},
) {
    val context = LocalContext.current
    val player = remember(url) {
        ExoPlayer.Builder(context).build().apply {
            setMediaItem(MediaItem.fromUri(url), startPositionMs)
            this.volume = volume
            prepare()
            playWhenReady = true
        }
    }
    var shown by remember(player) { mutableStateOf(audioInfo != null) }
    val onShownState by rememberUpdatedState(onShown)
    LaunchedEffect(player, shown) { onShownState(shown) }
    DisposableEffect(player) {
        var retriedDecoder = false
        val listener = object : Player.Listener {
            override fun onRenderedFirstFrame() {
                if (!shown) Log.i(TAG, "first frame: showing the URL video")
                shown = true
            }

            override fun onPlayerError(error: PlaybackException) {
                Log.w(TAG, "playback error: ${error.errorCodeName}", error)
                val decoder = error.errorCode == PlaybackException.ERROR_CODE_DECODER_INIT_FAILED ||
                    error.errorCode == PlaybackException.ERROR_CODE_DECODER_QUERY_FAILED
                if (decoder && !retriedDecoder) {
                    // The mirror may still hold the hardware decoder: release it and try once more.
                    retriedDecoder = true
                    shown = true
                    player.prepare()
                    return
                }
                shown = true  // show the message instead of a spinner over a frozen mirror
            }
        }
        player.addListener(listener)
        onDispose {
            player.removeListener(listener)
            player.release()
            onShownState(false)
        }
    }
    LaunchedEffect(player) {
        delay(SHOW_FALLBACK_MS)
        if (!shown) Log.i(TAG, "no frame after ${SHOW_FALLBACK_MS} ms: showing the player anyway")
        shown = true
    }
    LaunchedEffect(player) {
        commands.collect { cmd ->
            when (cmd) {
                is PlaybackCommand.Seek -> player.seekTo(cmd.positionMs)
                is PlaybackCommand.SetPlaying -> player.playWhenReady = cmd.playing
            }
        }
    }
    LaunchedEffect(player, volume) { player.volume = volume }
    var status by remember(player) { mutableStateOf(player.toStatus()) }
    LaunchedEffect(player) {
        val watchdog = StallWatchdog(player, url)
        while (isActive) {
            status = player.toStatus()
            onStatus(status)  // the sender polls /playback-info about once a second
            watchdog.check()
            delay(500)
        }
    }

    Box(Modifier.fillMaxSize().then(if (shown) Modifier.background(Color.Black) else Modifier)) {
        AndroidView(
            // Loading: full size (the decoder needs its surface) but off screen, so the mirror's
            // last frame stays visible underneath.
            modifier = Modifier.fillMaxSize().then(if (shown) Modifier else Modifier.offset { IntOffset(-20_000, 0) }),
            factory = {
                PlayerView(it).apply {
                    this.player = player
                    useController = true
                    setShutterBackgroundColor(android.graphics.Color.TRANSPARENT)
                }
            },
            // On failure our message replaces the (useless) transport controls.
            update = { it.useController = !status.failed },
        )
        if (!shown) LoadingRing(Modifier.align(Alignment.Center))
        if (status.failed) {
            Text(
                stringResource(R.string.media_failed),
                color = Color.White,
                modifier = Modifier.align(Alignment.Center).background(Color(0xCC000000), RoundedCornerShape(12.dp)).padding(24.dp),
            )
        }
        if (audioInfo != null && !status.failed) {
            val shownInfo = audioInfo.copy(
                positionMs = status.positionMs,
                durationMs = status.durationMs.takeIf { it > 0 } ?: audioInfo.durationMs,
                progressAtMs = SystemClock.elapsedRealtime(),
                playing = status.playing,
            )
            NowPlayingScreen(ui, clientName, shownInfo, onWatchAd)
        } else if (shown) {
            GraceBanner(ui, onWatchAd, Modifier.align(Alignment.BottomCenter).padding(24.dp))
        }
    }
}

/**
 * Stream stall recovery (from Vega, where the player sometimes stops for good in the middle of a
 * Safari HLS stream): if the player is meant to play but the position has stood still for 8 s,
 * reload the URL at the same position. At most 5 reloads per stall streak.
 */
private class StallWatchdog(private val player: ExoPlayer, private val url: String) {
    private var lastPos = -1L
    private var stillSince = SystemClock.elapsedRealtime()
    private var reloads = 0

    fun check() {
        val pos = player.currentPosition
        val now = SystemClock.elapsedRealtime()
        val waiting = player.playWhenReady && !player.isPlaying && player.playerError == null &&
            player.playbackState != Player.STATE_ENDED
        if (!waiting || pos != lastPos) {
            if (pos > lastPos + 10_000) reloads = 0  // playing again: later stalls get their retries
            lastPos = pos
            stillSince = now
            return
        }
        if (now - stillSince < STALL_MS || reloads >= MAX_RELOADS) return
        stillSince = now
        ++reloads
        Log.w(TAG, "stalled at ${pos / 1000.0} s: reloading ($reloads)")
        player.setMediaItem(MediaItem.fromUri(url), pos)
        player.prepare()
        player.playWhenReady = true
    }

    private companion object {
        const val STALL_MS = 8_000L
        const val MAX_RELOADS = 5
    }
}

/** A rotating ring over the mirror's frozen last frame while the video loads. */
@Composable
private fun LoadingRing(modifier: Modifier = Modifier) {
    val turn by rememberInfiniteTransition(label = "loading").animateFloat(
        initialValue = 0f,
        targetValue = 360f,
        animationSpec = infiniteRepeatable(tween(900, easing = LinearEasing)),
        label = "turn",
    )
    Box(modifier.background(Color(0x8C000000), RoundedCornerShape(16.dp)).padding(18.dp)) {
        Canvas(Modifier.size(48.dp).rotate(turn)) {
            val stroke = Stroke(width = 4.dp.toPx())
            drawArc(Color(0x40FFFFFF), 0f, 360f, useCenter = false, style = stroke)
            drawArc(Color.White, -90f, 90f, useCenter = false, style = stroke)
        }
    }
}

private const val TAG = "MediaUrlPlayer"
private const val SHOW_FALLBACK_MS = 2_500L

private fun Player.toStatus(): PlaybackStatus {
    val state = playbackState
    val dur = duration.takeIf { it != C.TIME_UNSET && it > 0 } ?: 0L
    return PlaybackStatus(
        durationMs = dur,
        positionMs = currentPosition.coerceAtLeast(0),
        playing = isPlaying || (playWhenReady && state == Player.STATE_BUFFERING),
        ready = state == Player.STATE_READY || state == Player.STATE_BUFFERING && dur > 0,
        buffering = state == Player.STATE_BUFFERING,
        ended = state == Player.STATE_ENDED,
        failed = playerError != null,
    )
}
