package app.screenhop.receiver.ui

import android.view.Surface
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
import app.screenhop.receiver.R
import kotlinx.coroutines.delay
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.media3.common.MediaItem
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.ui.PlayerView
import app.screenhop.core.session.ReceiverUiState

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

/** AirPlay "play this URL" and DLNA SetAVTransportURI both end up here. */
@Composable
fun MediaUrlPlayer(url: String, startPositionMs: Long, ui: ReceiverUiState, onWatchAd: () -> Unit) {
    val context = LocalContext.current
    val player = remember(url) {
        ExoPlayer.Builder(context).build().apply {
            setMediaItem(MediaItem.fromUri(url), startPositionMs)
            prepare()
            playWhenReady = true
        }
    }
    DisposableEffect(player) { onDispose { player.release() } }

    // TODO: report position/state back to the engine (AirPlay /playback-info, DLNA GetPositionInfo).
    Box(Modifier.fillMaxSize().background(Color.Black)) {
        AndroidView(
            modifier = Modifier.fillMaxSize(),
            factory = { PlayerView(it).apply { this.player = player; useController = true } },
        )
        GraceBanner(ui, onWatchAd, Modifier.align(Alignment.BottomCenter).padding(24.dp))
    }
}
