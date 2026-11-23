package com.screenatic.receiver.store

import android.app.Activity
import android.app.AlertDialog
import com.screenatic.core.monetization.RewardedAdProvider

/**
 * Placeholder ad: a dialog standing in for a 15–30 s rewarded video, so the
 * cast-time loop can be tested end to end before any ad SDK is wired in.
 */
class DevRewardedAd(private val label: String) : RewardedAdProvider {
    override val isReady = true
    override fun preload() = Unit
    override fun show(activity: Activity, onReward: () -> Unit, onClosed: () -> Unit) {
        AlertDialog.Builder(activity)
            .setTitle("[$label] Rewarded ad placeholder")
            .setMessage("A real 15–30 s rewarded video plays here.")
            .setPositiveButton("Finish ad") { _, _ -> onReward() }
            .setNegativeButton("Skip") { _, _ -> }
            .setOnDismissListener { onClosed() }
            .show()
    }
}
