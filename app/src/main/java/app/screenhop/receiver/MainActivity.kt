package app.screenhop.receiver

import android.os.Bundle
import android.view.KeyEvent
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import app.screenhop.core.receiver.SessionKind
import app.screenhop.receiver.service.ReceiverService
import app.screenhop.receiver.ui.DevTools
import app.screenhop.receiver.ui.HomeScreen
import app.screenhop.receiver.ui.ImageViewer
import app.screenhop.core.receiver.MediaType
import app.screenhop.receiver.ui.MediaUrlPlayer
import app.screenhop.receiver.ui.GraceBanner
import app.screenhop.receiver.ui.MirrorSurface
import app.screenhop.receiver.ui.NowPlayingScreen
import app.screenhop.receiver.ui.ZoomHint
import app.screenhop.receiver.ui.theme.ScreenHopTheme

/**
 * Single activity. Shows the idle/home screen, and swaps to the mirror surface or
 * the media player while a sender is connected. Staying in one activity avoids
 * Android 10+ background-activity-start limits when a session begins.
 */
class MainActivity : ComponentActivity() {

    /** Mirror zoom: false = fit (whole picture), true = fill (crop to cover the screen). */
    private var fillMode by mutableStateOf(false)
    /** Bumped on every toggle / session start to (re)show the zoom hint. */
    private var hintTick by mutableIntStateOf(0)
    private val prefs by lazy { getSharedPreferences("ui", MODE_PRIVATE) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        ReceiverService.start(this)
        val graph = graph
        fillMode = prefs.getBoolean("mirror_fill", false)

        setContent {
            ScreenHopTheme {
                val ui by graph.sessionManager.uiState.collectAsStateWithLifecycle()
                LaunchedEffect(Unit) { graph.ads.preload() }

                val active = ui.active
                Box(Modifier.fillMaxSize()) {
                    // Bottom layer, present whenever no URL video plays: the decoder needs a Surface
                    // before mirrored frames arrive. Left out during URL playback so ExoPlayer's
                    // own SurfaceView is never stacked under it.
                    if (active?.kind != SessionKind.MEDIA_URL) {
                        MirrorSurface(ui, fillMode, onSurface = { s -> graph.sessionManager.setVideoSurface(s) })
                    }
                    when {
                        active?.kind == SessionKind.MIRROR -> {
                            LaunchedEffect(active.clientName) { hintTick++ }
                            ZoomHint(fillMode, hintTick, Modifier.align(Alignment.TopCenter).padding(24.dp))
                            GraceBanner(ui, ::watchAd, Modifier.align(Alignment.BottomCenter).padding(24.dp))
                        }
                        active?.kind == SessionKind.AUDIO ->
                            NowPlayingScreen(ui, active.clientName, active.nowPlaying, onWatchAd = ::watchAd)
                        active?.kind == SessionKind.MEDIA_URL && active.mediaType == MediaType.IMAGE && active.mediaUrl != null ->
                            ImageViewer(active.mediaUrl!!, ui, onWatchAd = ::watchAd)
                        active?.kind == SessionKind.MEDIA_URL && active.mediaUrl != null ->
                            MediaUrlPlayer(
                                url = active.mediaUrl!!,
                                startPositionMs = active.startPositionMs,
                                ui = ui,
                                commands = graph.sessionManager.playbackCommands,
                                onStatus = graph.sessionManager::reportPlayback,
                                onWatchAd = ::watchAd,
                                volume = active.playerVolume,
                                audioInfo = active.nowPlaying.takeIf { active.mediaType == MediaType.AUDIO },
                                clientName = active.clientName,
                            )
                        else -> HomeScreen(  // opaque, covers the idle mirror surface
                            ui = ui,
                            deviceName = graph.receiverConfig.deviceName,
                            onWatchAd = ::watchAd,
                            devTools = if (BuildConfig.DEBUG) DevTools(
                                onSetUnlimited = { graph.setDevUnlimited(this@MainActivity, it) },
                                onResetBalance = graph.sessionManager::devResetBalance,
                            ) else null,
                        )
                    }
                }
            }
        }
    }

    /** Remote OK / Enter toggles fit and fill while mirroring. */
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val mirroring = graph.sessionManager.uiState.value.active?.kind == SessionKind.MIRROR
        val ok = event.keyCode == KeyEvent.KEYCODE_DPAD_CENTER || event.keyCode == KeyEvent.KEYCODE_ENTER
        if (mirroring && ok) {
            if (event.action == KeyEvent.ACTION_UP) {
                fillMode = !fillMode
                prefs.edit().putBoolean("mirror_fill", fillMode).apply()
                hintTick++
            }
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    private fun watchAd() {
        if (!graph.sessionManager.uiState.value.canEarnReward) return
        graph.ads.show(
            activity = this,
            onReward = { graph.sessionManager.onRewardEarned() },
            onClosed = { graph.ads.preload() },
        )
    }
}
