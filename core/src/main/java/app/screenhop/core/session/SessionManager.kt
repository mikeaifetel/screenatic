package app.screenhop.core.session

import android.view.Surface
import app.screenhop.core.receiver.EngineState
import app.screenhop.core.receiver.PlaybackCommand
import app.screenhop.core.receiver.PlaybackStatus
import app.screenhop.core.receiver.SessionKind
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.merge
import app.screenhop.core.receiver.ReceiverConfig
import app.screenhop.core.receiver.ReceiverEngine
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/** What the UI needs to render. */
data class ReceiverUiState(
    val active: EngineState.Connected? = null,
    val remainingSeconds: Long = 0,
    /** Seconds left in the grace window, non-null once the balance is empty during a session. */
    val graceSecondsLeft: Long? = null,
    /** False once today's rewarded-ad cap is reached. */
    val canEarnReward: Boolean = true,
    val rewardSeconds: Long = CastTimeRules().rewardSeconds,
    val idleAdsEnabled: Boolean = true,
    /** Dev builds: cast time is not metered. */
    val unlimited: Boolean = false,
)

/**
 * Owns all protocol engines, makes sure only one sender is active at a time,
 * and meters cast time while someone is connected.
 */
class SessionManager(
    private val engines: List<ReceiverEngine>,
    private val ledger: CastTimeLedger,
    private val rules: CastTimeRulesRepository,
    private val scope: CoroutineScope,
    private val rulesFetcher: RulesFetcher? = null,
    /** False in dev builds: casting never consumes or runs out of time. */
    private val meteringEnabled: Boolean = true,
) {
    private val grace = MutableStateFlow<Long?>(null)
    private var meterJob: Job? = null
    private var started = false

    private val activeSession: StateFlow<EngineState.Connected?> =
        combine(engines.map { it.state }) { states ->
            states.filterIsInstance<EngineState.Connected>().firstOrNull()
        }.stateIn(scope, SharingStarted.Eagerly, null)

    val uiState: StateFlow<ReceiverUiState> =
        combine(activeSession, ledger.snapshot, grace, rules.assignment) { active, snap, graceLeft, assignment ->
            val r = assignment.rules
            ReceiverUiState(
                active = active,
                remainingSeconds = snap.remainingSeconds,
                graceSecondsLeft = graceLeft,
                canEarnReward = r.dailyRewardCap <= 0 || snap.rewardsToday < r.dailyRewardCap,
                rewardSeconds = r.rewardSeconds,
                idleAdsEnabled = r.idleAdsEnabled,
                unlimited = !meteringEnabled,
            )
        }.stateIn(scope, SharingStarted.Eagerly, ReceiverUiState())

    private val _engineErrors = MutableStateFlow<List<String>>(emptyList())
    val engineErrors: StateFlow<List<String>> = _engineErrors.asStateFlow()

    /** Seek / pause / resume from whichever sender owns the MEDIA_URL session. */
    val playbackCommands: Flow<PlaybackCommand> = engines.map { it.playbackCommands }.merge()

    /** Player state -> the engine that started the MEDIA_URL session. */
    fun reportPlayback(status: PlaybackStatus) {
        engines.firstOrNull { (it.state.value as? EngineState.Connected)?.kind == SessionKind.MEDIA_URL }
            ?.reportPlayback(status)
    }

    /** Forward the mirror view's Surface to every engine (only the active one renders). */
    fun setVideoSurface(surface: Surface?) {
        engines.forEach { it.setVideoSurface(surface) }
    }

    fun start(config: ReceiverConfig) {
        if (started) return
        started = true
        scope.launch {
            rules.loadCached()
            ledger.ensureWelcomeGrant()
            rulesFetcher?.fetch()?.let { rules.update(it) }
        }
        engines.forEach { engine ->
            runCatching { engine.start(config) }
                .onFailure { _engineErrors.value += "${engine.protocol}: ${it.message}" }
        }
        scope.launch {
            activeSession.collect { active ->
                if (active != null) {
                    // One sender at a time: drop anyone connected on another protocol.
                    engines.filter { it.state.value is EngineState.Connected && it.state.value != active }
                        .forEach { it.disconnectClient() }
                    startMetering()
                } else {
                    stopMetering()
                }
            }
        }
    }

    fun stop() {
        started = false
        stopMetering()
        engines.forEach { it.stop() }
    }

    /** Called after a rewarded ad pays out. */
    fun onRewardEarned() {
        scope.launch {
            if (ledger.creditReward()) grace.value = null
            // TODO(analytics): log reward with rules.assignment.value.experimentId / variant.
        }
    }

    private fun startMetering() {
        if (!meteringEnabled || meterJob?.isActive == true) return
        meterJob = scope.launch {
            while (isActive) {
                delay(1_000)
                val left = ledger.debit(1)
                if (left > 0) {
                    grace.value = null
                    continue
                }
                val g = (grace.value ?: rules.rules.graceSeconds) - 1
                if (g > 0) {
                    grace.value = g
                } else {
                    grace.value = null
                    engines.forEach { it.disconnectClient() }
                }
            }
        }
    }

    private fun stopMetering() {
        meterJob?.cancel()
        meterJob = null
        grace.value = null
    }
}
