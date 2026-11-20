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

    /** This app's Google Play page: store app first, then the web page. */
    fun storePageUris(pkg: String): List<String> = listOf("market://details?id=$pkg", "https://play.google.com/store/apps/details?id=$pkg")
}
