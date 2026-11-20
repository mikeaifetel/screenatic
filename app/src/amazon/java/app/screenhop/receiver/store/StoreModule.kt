package app.screenhop.receiver.store

import android.content.Context
import app.screenhop.core.monetization.RewardedAdProvider

/**
 * Fire TV channel (Amazon Appstore, Fire OS).
 * TODO: rewarded video for Fire TV (e.g. Amazon Publisher Services, or SDK-less VAST in
 *       ExoPlayer — see README "Licensing"), idle-screen display ads.
 * Vega OS devices need a separate app (React Native); not covered by this module.
 */
object StoreModule {
    fun rewardedAds(context: Context): RewardedAdProvider = DevRewardedAd("amazon")
}
