package app.screenhop.receiver.store

import android.content.Context
import app.screenhop.core.monetization.RewardedAdProvider

/**
 * Google TV channel (Google Play).
 * TODO: rewarded video for CTV (e.g. VAST via IMA / Ad Manager, or SDK-less VAST in ExoPlayer —
 *       see README "Licensing"), idle-screen display ads.
 */
object StoreModule {
    fun rewardedAds(context: Context): RewardedAdProvider = DevRewardedAd("google")
}
