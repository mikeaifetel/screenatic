package com.screenatic.receiver.store

import android.content.Context
import com.screenatic.core.monetization.RewardedAdProvider

/**
 * Fire TV channel (Amazon Appstore, Fire OS).
 * TODO: rewarded video for Fire TV (e.g. Amazon Publisher Services, or SDK-less VAST in
 *       ExoPlayer — see README "Licensing"), idle-screen display ads.
 * Vega OS devices need a separate app (React Native); not covered by this module.
 */
object StoreModule {
    fun rewardedAds(context: Context): RewardedAdProvider = DevRewardedAd("amazon")

    /** This app's Amazon Appstore page: store app first, then the web page. */
    fun storePageUris(pkg: String): List<String> = listOf("amzn://apps/android?p=$pkg", "https://www.amazon.com/gp/mas/dl/android?p=$pkg")
}
