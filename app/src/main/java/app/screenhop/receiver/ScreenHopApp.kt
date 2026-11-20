package app.screenhop.receiver

import android.app.Application
import android.content.Context
import app.screenhop.airplay.AirPlayEngine
import app.screenhop.core.monetization.RewardedAdProvider
import app.screenhop.core.receiver.ReceiverConfig
import app.screenhop.core.session.CastTimeLedger
import app.screenhop.core.session.CastTimeRulesRepository
import app.screenhop.core.session.SessionManager
import app.screenhop.dlna.DlnaEngine
import app.screenhop.receiver.store.StoreModule
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import java.util.UUID

class ScreenHopApp : Application() {

    /** Manual DI: small enough that a framework isn't worth it yet. */
    lateinit var graph: AppGraph
        private set

    override fun onCreate() {
        super.onCreate()
        graph = AppGraph(this)
    }
}

class AppGraph(context: Context) {
    val appScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    val receiverConfig: ReceiverConfig = ReceiverConfig(
        deviceName = context.getString(R.string.default_device_name),
        deviceId = stableDeviceId(context),
    )

    /** Ad-for-time rules + A/B arm; refreshed from [BuildConfig.RULES_URL] when set. */
    val rules = CastTimeRulesRepository(context, receiverConfig.deviceId)
    val ledger = CastTimeLedger(context, rules)

    val sessionManager = SessionManager(
        engines = listOf(AirPlayEngine(context), DlnaEngine(context)),
        ledger = ledger,
        rules = rules,
        scope = appScope,
        rulesFetcher = BuildConfig.RULES_URL.takeIf { it.isNotBlank() }?.let { HttpRulesFetcher(it) },
        appVersionCode = BuildConfig.VERSION_CODE,
        // Dev builds: the home-screen switch decides; store builds: the remote rules do.
        devMetering = if (BuildConfig.DEBUG) !devPrefs(context).getBoolean(DEV_UNLIMITED, true) else null,
    )

    /** Dev builds: "unlimited cast time" switch on the home screen, remembered across restarts. */
    fun setDevUnlimited(context: Context, unlimited: Boolean) {
        if (!BuildConfig.DEBUG) return
        devPrefs(context).edit().putBoolean(DEV_UNLIMITED, unlimited).apply()
        sessionManager.setMetering(!unlimited)
    }

    private fun devPrefs(context: Context) = context.getSharedPreferences("dev", Context.MODE_PRIVATE)

    val ads: RewardedAdProvider = StoreModule.rewardedAds(context)

    private fun stableDeviceId(context: Context): String {
        val prefs = context.getSharedPreferences("device", Context.MODE_PRIVATE)
        prefs.getString("id", null)?.let { return it }
        // AirPlay wants a MAC-like id; derive a locally-administered one from a random UUID.
        val bytes = UUID.randomUUID().toString().replace("-", "").take(12).chunked(2)
            .map { it.toInt(16) }.toMutableList()
        bytes[0] = (bytes[0] or 0x02) and 0xFE
        val id = bytes.joinToString(":") { "%02X".format(it) }
        prefs.edit().putString("id", id).apply()
        return id
    }
}

private const val DEV_UNLIMITED = "unlimited"

val Context.graph: AppGraph get() = (applicationContext as ScreenHopApp).graph
