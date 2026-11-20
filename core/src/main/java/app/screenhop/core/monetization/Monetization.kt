package app.screenhop.core.monetization

import android.app.Activity

/**
 * ScreenHop's only revenue: rewarded ads that buy cast time (plus idle-screen ads).
 * Store-specific ad SDKs live in the app's `google` / `amazon` flavors.
 */
interface RewardedAdProvider {
    /** Preload so the ad starts instantly when the user asks for more time. */
    fun preload()

    /**
     * Show a rewarded video. [onReward] fires only when the ad network says the
     * reward is earned. The server credit must come from SSV, not this callback.
     */
    fun show(activity: Activity, onReward: () -> Unit, onClosed: () -> Unit)

    val isReady: Boolean
}
