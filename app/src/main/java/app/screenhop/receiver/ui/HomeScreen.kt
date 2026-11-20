package app.screenhop.receiver.ui

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
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.tv.material3.Button
import androidx.tv.material3.MaterialTheme
import androidx.tv.material3.Text
import app.screenhop.core.session.ReceiverUiState
import app.screenhop.receiver.R
import java.net.Inet4Address
import java.net.NetworkInterface

@Composable
fun HomeScreen(
    ui: ReceiverUiState,
    deviceName: String,
    onWatchAd: () -> Unit,
) {
    val focus = remember { FocusRequester() }
    LaunchedEffect(Unit) { runCatching { focus.requestFocus() } }

    Row(
        Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .padding(48.dp),
        horizontalArrangement = Arrangement.spacedBy(48.dp),
    ) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.Center) {
            Text(stringResource(R.string.home_title), style = MaterialTheme.typography.displaySmall)
            Spacer(Modifier.height(16.dp))
            Text(deviceName, style = MaterialTheme.typography.headlineMedium, color = MaterialTheme.colorScheme.primary)
            Spacer(Modifier.height(16.dp))
            Text(stringResource(R.string.home_hint, deviceName), style = MaterialTheme.typography.bodyLarge)
            localIpv4()?.let {
                Spacer(Modifier.height(8.dp))
                Text(stringResource(R.string.home_ip, it), style = MaterialTheme.typography.bodyMedium)
            }
            Spacer(Modifier.height(32.dp))
            CastTimeLine(ui)
            Spacer(Modifier.height(16.dp))
            WatchAdButton(ui, onWatchAd, Modifier.focusRequester(focus))
        }
        // Idle-screen ad slot (display ad / house promo). Never shown during a session.
        if (ui.idleAdsEnabled) Box(
            Modifier
                .weight(1f)
                .fillMaxWidth()
                .background(MaterialTheme.colorScheme.surface),
            contentAlignment = Alignment.Center,
        ) {
            Text("Ad slot", color = Color.Gray)
        }
    }
}

@Composable
fun CastTimeLine(ui: ReceiverUiState) {
    val text = if (ui.unlimited) stringResource(R.string.time_dev_unlimited)
    else stringResource(R.string.time_left, formatDuration(ui.remainingSeconds))
    Text(text, style = MaterialTheme.typography.titleMedium)
}

@Composable
fun WatchAdButton(ui: ReceiverUiState, onWatchAd: () -> Unit, modifier: Modifier = Modifier) {
    if (ui.canEarnReward) {
        Button(onClick = onWatchAd, modifier = modifier) {
            Text(stringResource(R.string.watch_ad, (ui.rewardSeconds / 60).toInt()))
        }
    } else {
        Text(stringResource(R.string.reward_cap_reached), style = MaterialTheme.typography.bodyMedium)
    }
}

/** Shown on top of a running session when the balance is used up. */
@Composable
fun GraceBanner(ui: ReceiverUiState, onWatchAd: () -> Unit, modifier: Modifier = Modifier) {
    val left = ui.graceSecondsLeft ?: return
    Row(
        modifier
            .background(Color(0xCC000000))
            .padding(16.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Text(stringResource(R.string.grace_banner, formatDuration(left)), color = Color.White)
        WatchAdButton(ui, onWatchAd)
    }
}

fun formatDuration(seconds: Long): String {
    val h = seconds / 3600
    val m = (seconds % 3600) / 60
    val s = seconds % 60
    return if (h > 0) "%d:%02d:%02d".format(h, m, s) else "%d:%02d".format(m, s)
}

private fun localIpv4(): String? = runCatching {
    NetworkInterface.getNetworkInterfaces().toList()
        .filter { it.isUp && !it.isLoopback }
        .flatMap { it.inetAddresses.toList() }
        .firstOrNull { it is Inet4Address }?.hostAddress
}.getOrNull()
