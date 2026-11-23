package com.screenatic.core.session

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class CastTimeRulesTest {

    private val config = """
        {
          "version": 3,
          "defaults": { "rewardMin": 60, "graceSec": 90, "dailyRewardCap": 10 },
          "experiment": {
            "id": "reward_len",
            "variants": [
              { "name": "control", "weight": 50 },
              { "name": "r45", "weight": 50, "rules": { "rewardMin": 45 } }
            ]
          }
        }
    """.trimIndent()

    @Test
    fun defaultsApplyWithoutExperiment() {
        val a = CastTimeRulesRepository.resolve(JSONObject("""{"defaults":{"rewardMin":30}}"""), "dev")
        assertEquals(30L * 60, a.rules.rewardSeconds)
        assertNull(a.experimentId)
    }

    @Test
    fun freeByDefaultAndMinVersionParsed() {
        val none = CastTimeRulesRepository.resolve(JSONObject("{}"), "dev")
        assertEquals(false, none.rules.metering)
        assertEquals(0, none.minVersionCode)
        val a = CastTimeRulesRepository.resolve(JSONObject("""{"minVersionCode":7,"defaults":{"metering":true}}"""), "dev")
        assertEquals(true, a.rules.metering)
        assertEquals(7, a.minVersionCode)
    }

    /** Growing the first arm's weight only adds devices to it; nobody already metered flips back. */
    @Test
    fun rolloutGrowsWithoutReshuffling() {
        fun cfg(pct: Int) = JSONObject("""{"experiment":{"id":"metering_rollout","variants":[
            {"name":"metered","weight":$pct,"rules":{"metering":true}},
            {"name":"free","weight":${100 - pct}}]}}""")
        val ids = (0 until 2000).map { "02:00:00:00:%04X".format(it) }
        val at10 = ids.filter { CastTimeRulesRepository.resolve(cfg(10), it).rules.metering }.toSet()
        val at30 = ids.filter { CastTimeRulesRepository.resolve(cfg(30), it).rules.metering }.toSet()
        assert(at30.containsAll(at10))
        assert(at10.size in 120..280) { "10% arm: ${at10.size}/2000" }
        assert(at30.size in 480..720) { "30% arm: ${at30.size}/2000" }
    }

    @Test
    fun bucketingIsStableAndCoversBothArms() {
        val arms = (0 until 1000).map {
            val id = "02:00:00:00:%04X".format(it)
            val first = CastTimeRulesRepository.resolve(JSONObject(config), id)
            val again = CastTimeRulesRepository.resolve(JSONObject(config), id)
            assertEquals(first, again)
            assertEquals(90L, first.rules.graceSeconds)
            assertEquals(10, first.rules.dailyRewardCap)
            if (first.variant == "r45") assertEquals(45L * 60, first.rules.rewardSeconds)
            else assertEquals(60L * 60, first.rules.rewardSeconds)
            first.variant
        }
        val r45 = arms.count { it == "r45" }
        assert(r45 in 350..650) { "unbalanced split: $r45/1000" }
    }
}
