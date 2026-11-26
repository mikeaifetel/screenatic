package com.screenatic.core.session

import android.view.Surface
import com.screenatic.core.receiver.EngineState
import com.screenatic.core.receiver.PlaybackCommand
import com.screenatic.core.receiver.PlaybackStatus
import com.screenatic.core.receiver.SessionKind
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.merge
import com.screenatic.core.receiver.ReceiverConfig
import com.screenatic.core.receiver.ReceiverEngine
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
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
    val sponsorSlotEnabled: Boolean = false,
    /** Cast time is not metered right now (remote rules say free, or the dev switch). Hide ad UI. */
    val unlimited: Boolean = false,
    /** Dev builds: the "unlimited" dev switch is on. */
    val devUnlimited: Boolean = false,
    /** This build is below the remote minVersionCode: show the update screen, receivers are off. */
    val updateRequired: Boolean = false,
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
    /** This build's versionCode, compared with the remote minVersionCode. */
    private val appVersionCode: Int = 0,
    /**
     * Dev builds: initial state of the metering dev switch, which overrides the remote rules (see
     * [setMetering]). Null in store builds: the remote rules alone decide.
     */
    devMetering: Boolean? = null,
) {
    private val devSwitch = MutableStateFlow(devMetering)
    private var config: ReceiverConfig? = null

    private fun meteringOn(a: RulesAssignment, dev: Boolean?) = dev ?: a.rules.metering
    private fun updateRequired(a: RulesAssignment) = a.minVersionCode > appVersionCode
    private val grace = MutableStateFlow<Long?>(null)
    private var meterJob: Job? = null
    private var started = false

    private val activeSession: StateFlow<EngineState.Connected?> =
        combine(engines.map { it.state }) { states ->
            states.filterIsInstance<EngineState.Connected>().firstOrNull()
        }.stateIn(scope, SharingStarted.Eagerly, null)

    val uiState: StateFlow<ReceiverUiState> =
        combine(activeSession, ledger.snapshot, grace, rules.assignment, devSwitch) { active, snap, graceLeft, assignment, dev ->
            val r = assignment.rules
            ReceiverUiState(
                active = active,
                remainingSeconds = snap.remainingSeconds,
                graceSecondsLeft = graceLeft,
                canEarnReward = r.dailyRewardCap <= 0 || snap.rewardsToday < r.dailyRewardCap,
                rewardSeconds = r.rewardSeconds,
                sponsorSlotEnabled = r.sponsorSlotEnabled,
                unlimited = !meteringOn(assignment, dev),
                devUnlimited = dev == false,
                updateRequired = updateRequired(assignment),
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
        this.config = config
        scope.launch {
            rules.loadCached()
            ledger.ensureWelcomeGrant()
            // The TV app runs for days: refresh the remote rules periodically, not just at start.
            while (isActive) {
                rulesFetcher?.fetch()?.let { rules.update(it) }
                delay(RULES_REFRESH_MS)
            }
        }
        // Receivers run unless this build is too old (then senders shouldn't even see the TV).
        scope.launch {
            rules.assignment.map { updateRequired(it) }.distinctUntilChanged().collect { tooOld ->
                if (tooOld) engines.forEach { it.stop() } else startEngines()
            }
        }
        // One sender at a time: whoever connects last wins, senders on other protocols are dropped.
        engines.forEach { engine ->
            scope.launch {
                engine.state.map { it is EngineState.Connected }.distinctUntilChanged().collect { connected ->
                    if (connected) {
                        engines.filter { it !== engine && it.state.value is EngineState.Connected }
                            .forEach { it.disconnectClient() }
                    }
                }
            }
        }
        scope.launch {
            combine(activeSession, rules.assignment, devSwitch) { active, a, dev -> active != null && meteringOn(a, dev) }
                .distinctUntilChanged()
                .collect { meter -> if (meter) startMetering() else stopMetering() }
        }
    }

    private fun startEngines() {
        val c = config ?: return
        engines.filter { it.state.value is EngineState.Stopped }.forEach { engine ->
            runCatching { engine.start(c) }
                .onFailure { _engineErrors.value += "${engine.protocol}: ${it.message}" }
        }
    }

    fun stop() {
        started = false
        stopMetering()
        engines.forEach { it.stop() }
    }

    /** Dev builds: turn cast-time metering on/off regardless of the remote rules, effective immediately. */
    fun setMetering(enabled: Boolean) {
        devSwitch.value = enabled
    }

    /** Dev builds: empty the balance and today's reward count to test running out / the ad flow. */
    fun devResetBalance() {
        scope.launch { ledger.devReset() }
    }

    /** Called after a rewarded ad pays out. */
    fun onRewardEarned() {
        scope.launch {
            if (ledger.creditReward()) grace.value = null
            // TODO(analytics): log reward with rules.assignment.value.experimentId / variant.
        }
    }

    private fun startMetering() {
        if (meterJob?.isActive == true) return
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

    private companion object {
        const val RULES_REFRESH_MS = 6L * 60 * 60 * 1000
    }
}
