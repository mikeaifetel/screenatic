package com.screenatic.receiver.sponsor

import android.content.Context
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL

/**
 * Sponsor reach: how many times each item was on screen for at least a second. Counted on the
 * device and reported in one batch with each sponsor-file refresh (app start, then every few hours)
 * to screenatic.com/px (site/worker.js), which keeps only aggregate counts per item, country and
 * day. No device identifier is sent. QR scans are counted by the site's /c/{id} redirect.
 */
object SponsorStats {
    private const val TAG = "SponsorStats"
    private const val PREFS = "sponsor_stats"
    private const val KEY = "counts"
    private const val URL_PX = "${Sponsors.SITE}/px"

    @Synchronized
    fun count(context: Context, id: String) {
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val counts = runCatching { JSONObject(prefs.getString(KEY, "{}")!!) }.getOrDefault(JSONObject())
        counts.put(id, counts.optInt(id) + 1)
        prefs.edit().putString(KEY, counts.toString()).apply()
    }

    /** Sends the counts so far; they're kept (and sent next time) if the report fails. */
    suspend fun flush(context: Context, build: Int, dev: Boolean) {
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val sent = synchronized(this) { prefs.getString(KEY, "{}")!! }
        val counts = runCatching { JSONObject(sent) }.getOrNull() ?: return
        if (counts.length() == 0) return
        val body = JSONObject()
            .put("file", if (dev) "dev" else "store")
            .put("app", "android")
            .put("build", build)
            .put("counts", counts)
            .toString()
        val ok = withContext(Dispatchers.IO) {
            runCatching {
                val conn = URL(URL_PX).openConnection() as HttpURLConnection
                conn.requestMethod = "POST"
                conn.doOutput = true
                conn.connectTimeout = 5_000
                conn.readTimeout = 5_000
                conn.setRequestProperty("Content-Type", "application/json")
                try {
                    conn.outputStream.use { it.write(body.toByteArray()) }
                    conn.responseCode == 204
                } finally {
                    conn.disconnect()
                }
            }.onFailure { Log.w(TAG, "report failed: $it") }.getOrDefault(false)
        }
        if (!ok) return
        synchronized(this) {
            // Keep what was counted while the report was on its way.
            val now = runCatching { JSONObject(prefs.getString(KEY, "{}")!!) }.getOrDefault(JSONObject())
            val left = JSONObject()
            now.keys().forEach { id ->
                val n = now.optInt(id) - counts.optInt(id)
                if (n > 0) left.put(id, n)
            }
            prefs.edit().putString(KEY, left.toString()).apply()
        }
    }
}
