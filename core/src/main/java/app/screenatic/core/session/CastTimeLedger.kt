package app.screenatic.core.session

import android.content.Context
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.longPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import java.util.TimeZone

private val Context.ledgerStore by preferencesDataStore(name = "cast_time_ledger")

data class LedgerSnapshot(
    val remainingSeconds: Long,
    val rewardsToday: Int,
)

/**
 * Local cast-time balance. Amounts and caps come from [CastTimeRulesRepository].
 *
 * TODO(server): this is a local cache only. The source of truth must be the backend,
 * crediting rewards from ad-network server-side verification (SSV) callbacks.
 */
class CastTimeLedger(
    private val context: Context,
    private val rules: CastTimeRulesRepository,
) {
    private object Keys {
        val remaining = longPreferencesKey("remaining_seconds")
        val welcomed = booleanPreferencesKey("welcome_granted")
        val rewardDay = longPreferencesKey("reward_day")
        val rewardsToday = intPreferencesKey("rewards_today")
    }

    val snapshot: Flow<LedgerSnapshot> = context.ledgerStore.data.map {
        val today = localEpochDay()
        LedgerSnapshot(
            remainingSeconds = it[Keys.remaining] ?: 0L,
            rewardsToday = if (it[Keys.rewardDay] == today) it[Keys.rewardsToday] ?: 0 else 0,
        )
    }

    suspend fun ensureWelcomeGrant() {
        context.ledgerStore.edit {
            if (it[Keys.welcomed] != true) {
                it[Keys.welcomed] = true
                it[Keys.remaining] = rules.rules.welcomeGrantSeconds
            }
        }
    }

    /** True if another rewarded ad is allowed today under the current rules. */
    suspend fun canEarnReward(): Boolean {
        val cap = rules.rules.dailyRewardCap
        return cap <= 0 || snapshot.first().rewardsToday < cap
    }

    /** Credit one rewarded ad. Returns false if the daily cap is reached. */
    suspend fun creditReward(): Boolean {
        val r = rules.rules
        var credited = false
        context.ledgerStore.edit {
            val today = localEpochDay()
            val count = if (it[Keys.rewardDay] == today) it[Keys.rewardsToday] ?: 0 else 0
            if (r.dailyRewardCap > 0 && count >= r.dailyRewardCap) return@edit
            val now = it[Keys.remaining] ?: 0L
            it[Keys.remaining] = (now + r.rewardSeconds).coerceAtMost(r.maxBalanceSeconds)
            it[Keys.rewardDay] = today
            it[Keys.rewardsToday] = count + 1
            credited = true
        }
        return credited
    }

    /** Dev builds only: zero the balance and today's rewarded-ad count. */
    suspend fun devReset() {
        context.ledgerStore.edit {
            it[Keys.remaining] = 0L
            it[Keys.rewardsToday] = 0
        }
    }

    /** Debit [seconds]; returns the balance left. */
    suspend fun debit(seconds: Long): Long {
        var left = 0L
        context.ledgerStore.edit {
            left = ((it[Keys.remaining] ?: 0L) - seconds).coerceAtLeast(0L)
            it[Keys.remaining] = left
        }
        return left
    }
}

/** Local calendar day number (java.time needs API 26; minSdk is 25). */
private fun localEpochDay(): Long {
    val now = System.currentTimeMillis()
    return (now + TimeZone.getDefault().getOffset(now)) / 86_400_000L
}
