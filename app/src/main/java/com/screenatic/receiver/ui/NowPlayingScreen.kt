package com.screenatic.receiver.ui

import android.graphics.BitmapFactory
import android.os.SystemClock
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.tv.material3.MaterialTheme
import androidx.tv.material3.Text
import com.screenatic.core.receiver.NowPlaying
import com.screenatic.core.session.ReceiverUiState
import com.screenatic.receiver.R
import kotlinx.coroutines.delay

/** Audio sessions (AirPlay Music/Podcasts, DLNA music): cover art, track info and a progress bar. */
@Composable
fun NowPlayingScreen(ui: ReceiverUiState, clientName: String, nowPlaying: NowPlaying?, onWatchAd: () -> Unit) {
    val np = nowPlaying ?: NowPlaying()
    val artFromBytes = remember(np.artwork) {
        np.artwork?.let { runCatching { BitmapFactory.decodeByteArray(it, 0, it.size)?.asImageBitmap() }.getOrNull() }
    }
    val artFromUrl by produceState<ImageBitmap?>(null, np.artworkUrl) {
        value = np.artworkUrl?.let { runCatching { loadBitmap(it, 600, 600)?.asImageBitmap() }.getOrNull() }
    }
    val art = artFromBytes ?: artFromUrl
    // The sender only reports progress on track change / seek; interpolate in between.
    var now by remember { mutableLongStateOf(SystemClock.elapsedRealtime()) }
    LaunchedEffect(Unit) {
        while (true) {
            delay(500)
            now = SystemClock.elapsedRealtime()
        }
    }
    val position = if (np.durationMs > 0) np.positionAt(now) else 0L

    Box(Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background)) {
        Row(
            Modifier.fillMaxSize().padding(horizontal = 96.dp, vertical = 72.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(64.dp),
        ) {
            val artModifier = Modifier.size(420.dp).clip(RoundedCornerShape(16.dp))
            if (art != null) {
                Image(art, contentDescription = null, modifier = artModifier, contentScale = ContentScale.Crop)
            } else {
                Box(artModifier.background(MaterialTheme.colorScheme.surface), contentAlignment = Alignment.Center) {
                    Image(painterResource(R.drawable.ic_launcher_foreground), contentDescription = null, Modifier.size(240.dp))
                }
            }
            Column(Modifier.weight(1f)) {
                Text(
                    stringResource(R.string.now_playing_from, clientName),
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.primary,
                )
                Spacer(Modifier.height(16.dp))
                Text(
                    np.title.ifBlank { stringResource(R.string.now_playing_unknown) },
                    style = MaterialTheme.typography.displaySmall,
                    maxLines = 2, overflow = TextOverflow.Ellipsis,
                )
                if (np.artist.isNotBlank()) {
                    Spacer(Modifier.height(12.dp))
                    Text(np.artist, style = MaterialTheme.typography.headlineSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
                if (np.album.isNotBlank()) {
                    Spacer(Modifier.height(4.dp))
                    Text(np.album, style = MaterialTheme.typography.titleLarge, color = Color.Gray, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
                if (np.durationMs > 0) {
                    Spacer(Modifier.height(40.dp))
                    Box(Modifier.fillMaxWidth().height(6.dp).clip(RoundedCornerShape(3.dp)).background(Color(0x33FFFFFF))) {
                        Box(
                            Modifier.fillMaxWidth(position.toFloat() / np.durationMs).height(6.dp)
                                .background(MaterialTheme.colorScheme.primary),
                        )
                    }
                    Spacer(Modifier.height(8.dp))
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                        Text(formatDuration(position / 1000), style = MaterialTheme.typography.bodyMedium)
                        Text(formatDuration(np.durationMs / 1000), style = MaterialTheme.typography.bodyMedium)
                    }
                }
                Spacer(Modifier.height(32.dp))
                CastTimeLine(ui)
            }
        }
        GraceBanner(ui, onWatchAd, Modifier.align(Alignment.BottomCenter).padding(24.dp))
    }
}
