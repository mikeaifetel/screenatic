package com.screenatic.receiver

import android.content.Intent
import android.net.Uri
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
import com.screenatic.core.receiver.SessionKind
import com.screenatic.receiver.service.ReceiverService
import com.screenatic.receiver.ui.DevTools
import com.screenatic.receiver.ui.HomeScreen
import com.screenatic.receiver.ui.UpdateScreen
import com.screenatic.receiver.store.StoreModule
import com.screenatic.receiver.ui.ImageViewer
import com.screenatic.core.receiver.MediaType
import com.screenatic.receiver.ui.MediaUrlPlayer
import com.screenatic.receiver.ui.GraceBanner
import com.screenatic.receiver.ui.MirrorSurface
import com.screenatic.receiver.ui.NowPlayingScreen
import com.screenatic.receiver.ui.ZoomHint
import com.screenatic.receiver.ui.theme.ScreenaticTheme

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
    /** The URL video (Safari fullscreen, YouTube...) shows frames; until then the mirror stays. */
    private var urlVideoShown by mutableStateOf(false)
    private val prefs by lazy { getSharedPreferences("ui", MODE_PRIVATE) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        ReceiverService.start(this)
        val graph = graph
        fillMode = prefs.getBoolean("mirror_fill", false)

        setContent {
            ScreenaticTheme {
                val ui by graph.sessionManager.uiState.collectAsStateWithLifecycle()
                LaunchedEffect(Unit) { graph.ads.preload() }

                val active = ui.active
                Box(Modifier.fillMaxSize()) {
                    // Bottom layer, present whenever no URL video is on screen: the decoder needs a
                    // Surface before mirrored frames arrive. While a URL video loads it keeps the
                    // mirror's last frame visible (ExoPlayer's view waits off screen); once the video
                    // shows, it's left out so ExoPlayer's own SurfaceView is never stacked with it,
                    // and the mirror decoder is released.
                    if (active?.kind != SessionKind.MEDIA_URL || !urlVideoShown) {
                        MirrorSurface(ui, fillMode, onSurface = { s -> graph.sessionManager.setVideoSurface(s) })
                    }
                    when {
                        ui.updateRequired -> UpdateScreen(onOpenStore = ::openStorePage)
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
                                onShown = { urlVideoShown = it },
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

    private fun openStorePage() {
        for (uri in StoreModule.storePageUris(BuildConfig.APPLICATION_ID)) {
            val opened = runCatching {
                startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(uri)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
            }.isSuccess
            if (opened) return
        }
    }

    /** Remote OK / Enter toggles fit and fill while mirroring. */
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val ui = graph.sessionManager.uiState.value
        val mirroring = ui.active?.kind == SessionKind.MIRROR
        val ok = event.keyCode == KeyEvent.KEYCODE_DPAD_CENTER || event.keyCode == KeyEvent.KEYCODE_ENTER
        // Cast time is up (grace banner over a session): OK opens the ad, whatever has the focus
        // (over the mirror nothing can take it). Same rule as the Vega app.
        if (ok && ui.active != null && ui.graceSecondsLeft != null && ui.canEarnReward) {
            if (event.action == KeyEvent.ACTION_UP) watchAd()
            return true
        }
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
