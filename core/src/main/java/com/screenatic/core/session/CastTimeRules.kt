package com.screenatic.core.session

import android.content.Context
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import org.json.JSONObject

/** Tunable "ads for cast time" rules. Every field can be changed remotely. */
data class CastTimeRules(
    val welcomeGrantSeconds: Long = 30L * 60,
    val rewardSeconds: Long = 60L * 60,
    val maxBalanceSeconds: Long = 8L * 60 * 60,
    /** Casting continues this long after the balance hits zero, so the user can watch an ad. */
    val graceSeconds: Long = 2L * 60,
    /** Max rewarded ads per local day; 0 = unlimited. */
    val dailyRewardCap: Int = 0,
    /** Show the display-ad slot on the idle screen (off until an ad source exists). */
    val idleAdsEnabled: Boolean = false,
    /**
     * Cast time is metered (and rewarded ads offered). Off = casting is free and no ad UI shows.
     * Roll out gradually with an experiment whose growing arm is listed first (see README).
     */
    val metering: Boolean = false,
) {
    /** Override fields present in [o] (minutes/seconds as named in the JSON). */
    fun overriddenBy(o: JSONObject?): CastTimeRules {
        if (o == null) return this
        return copy(
            welcomeGrantSeconds = o.optMinutes("welcomeGrantMin", welcomeGrantSeconds),
            rewardSeconds = o.optMinutes("rewardMin", rewardSeconds),
            maxBalanceSeconds = o.optMinutes("maxBalanceMin", maxBalanceSeconds),
            graceSeconds = if (o.has("graceSec")) o.getLong("graceSec") else graceSeconds,
            dailyRewardCap = o.optInt("dailyRewardCap", dailyRewardCap),
            idleAdsEnabled = o.optBoolean("idleAds", idleAdsEnabled),
            metering = o.optBoolean("metering", metering),
        )
    }

    private fun JSONObject.optMinutes(key: String, fallbackSeconds: Long) =
        if (has(key)) getLong(key) * 60 else fallbackSeconds
}

/** Which rules this device runs, and which experiment arm it belongs to (log it with every ad event). */
data class RulesAssignment(
    val rules: CastTimeRules = CastTimeRules(),
    val configVersion: Int = 0,
    val experimentId: String? = null,
    val variant: String = "default",
    /** Builds below this versionCode must update before casting (top-level "minVersionCode"). */
    val minVersionCode: Int = 0,
)

private val Context.rulesStore by preferencesDataStore(name = "cast_time_rules")
private val CONFIG_JSON = stringPreferencesKey("config_json")

/**
 * Holds the active [RulesAssignment].
 *
 * Config JSON (served by our backend / remote config; cached locally):
 * ```
 * {
 *   "version": 3,
 *   "minVersionCode": 1,
 *   "defaults": { "metering": false, "welcomeGrantMin": 30, "rewardMin": 60, "maxBalanceMin": 480,
 *                 "graceSec": 120, "dailyRewardCap": 0, "idleAds": false },
 *   "experiment": {
 *     "id": "reward_len_2026_12",
 *     "variants": [
 *       { "name": "control", "weight": 50 },
 *       { "name": "r45",     "weight": 50, "rules": { "rewardMin": 45 } }
 *     ]
 *   }
 * }
 * ```
 * Devices are bucketed deterministically by (experiment id, device id), so a device
 * stays in the same arm across restarts and config refreshes.
 */
class CastTimeRulesRepository(
    private val context: Context,
    private val deviceId: String,
) {
    private val _assignment = MutableStateFlow(RulesAssignment())
    val assignment: StateFlow<RulesAssignment> = _assignment.asStateFlow()

    val rules: CastTimeRules get() = _assignment.value.rules

    /** Apply the last cached config (call once at startup, before metering). */
    suspend fun loadCached() {
        context.rulesStore.data.first()[CONFIG_JSON]?.let { apply(it) }
    }

    /** Apply and cache a freshly fetched config. Invalid JSON is ignored and the old rules stay. */
    suspend fun update(configJson: String) {
        if (apply(configJson)) {
            context.rulesStore.edit { it[CONFIG_JSON] = configJson }
        }
    }

    private fun apply(json: String): Boolean = runCatching {
        _assignment.value = resolve(JSONObject(json), deviceId)
    }.isSuccess

    companion object {
        fun resolve(config: JSONObject, deviceId: String): RulesAssignment {
            val base = CastTimeRules().overriddenBy(config.optJSONObject("defaults"))
            val version = config.optInt("version", 0)
            val minVersion = config.optInt("minVersionCode", 0)
            return pick(config, base, version, deviceId).copy(minVersionCode = minVersion)
        }

        private fun pick(config: JSONObject, base: CastTimeRules, version: Int, deviceId: String): RulesAssignment {
            val experiment = config.optJSONObject("experiment")
                ?: return RulesAssignment(base, version)
            val id = experiment.getString("id")
            val variants = experiment.getJSONArray("variants")
            val weights = (0 until variants.length()).map { variants.getJSONObject(it).optInt("weight", 0) }
            val total = weights.sum()
            if (total <= 0) return RulesAssignment(base, version)

            var bucket = Math.floorMod("$id:$deviceId".hashCode(), total)
            for (i in 0 until variants.length()) {
                if (bucket < weights[i]) {
                    val v = variants.getJSONObject(i)
                    return RulesAssignment(
                        rules = base.overriddenBy(v.optJSONObject("rules")),
                        configVersion = version,
                        experimentId = id,
                        variant = v.getString("name"),
                    )
                }
                bucket -= weights[i]
            }
            return RulesAssignment(base, version)
        }
    }
}

/** Where the config comes from. Implementations: our backend, Firebase Remote Config, etc. */
fun interface RulesFetcher {
    /** Returns the config JSON, or null if unavailable (keep cached rules). */
    suspend fun fetch(): String?
}
