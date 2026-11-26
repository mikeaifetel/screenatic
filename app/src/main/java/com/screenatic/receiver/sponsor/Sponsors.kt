package com.screenatic.receiver.sponsor

import android.content.Context
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone

/**
 * Self-run sponsor slot on the idle home screen (no ad SDK). The content is a small JSON on
 * screenatic.com (docs/sponsors.json; debug builds read sponsors-dev.json), the same file the
 * Vega app reads (vega/app/src/sponsors.ts):
 *
 * { "version": 1, "items": [ {
 *     "id": "house-beta",            unique, stable; a-z, 0-9 and "-", up to 40 (it is in the QR URL)
 *     "kind": "house" | "sponsored", label: "From Screenatic" or the localized "Sponsored"
 *     "title": { "en": "...", "zh-CN": "..." },   localized, falls back to en
 *     "body":  { "en": "..." },                   optional
 *     "qr": "https://...",           optional target of the QR code; the code itself is
 *                                    screenatic.com/c/{id}, which counts the scan and redirects
 *     "image": "https://....png",    optional, PNG/JPEG
 *     "languages": ["en", "zh-CN"],  optional, our language tags (as in the app's translations)
 *     "regions": ["US"], "excludeRegions": ["CN"],   optional, country (ISO 3166, see below)
 *     "start": "2026-12-01", "end": "2026-12-31",   optional, UTC dates, inclusive
 *     "weight": 1                    rotation weight
 * } ],
 *   "country": "CA"   added by the site's worker (site/worker.js): the country of the connection
 * }
 *
 * Country for regions: the one the server saw (Cloudflare, from the IP), else the device's
 * language-region setting (often en-US whatever the country). Nothing is sent to the server: the whole file is fetched and filtered on the device by its
 * language, region and date. Anything invalid is dropped; no valid item = no slot (the logo shows).
 *
 * Text length: write titles within 40 units and texts within 150 (a CJK / full-width character
 * counts 2), so they fit the card's 2 and 4 lines in every language; scripts/check-sponsors.py checks
 * this before a push. On screen, longer text is cut at [MAX_TITLE] / [MAX_BODY] characters and
 * ellipsized at the line limit.
 */
data class Sponsor(
    val id: String,
    val house: Boolean,
    val title: String,
    val body: String?,
    val qr: String?,
    val image: String?,
    val weight: Int,
)

object Sponsors {
    private const val TAG = "Sponsors"
    private const val PREFS = "sponsors"
    private const val KEY_JSON = "json"
    private const val KEY_AT = "fetched_at"
    const val SITE = "https://screenatic.com"
    private val ID = Regex("^[a-z0-9][a-z0-9-]{0,39}$")
    private const val MAX_BYTES = 64 * 1024
    const val MAX_TITLE = 80
    const val MAX_BODY = 300
    /** Same pace as the rules: the TV app runs for days. */
    const val REFRESH_MS = 6 * 60 * 60 * 1000L

    /** Cached items for this device, refreshed from [url] when the cache is older than [REFRESH_MS]. */
    suspend fun load(context: Context, url: String, force: Boolean = false): List<Sponsor> {
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val age = System.currentTimeMillis() - prefs.getLong(KEY_AT, 0)
        var json = prefs.getString(KEY_JSON, null)
        if (force || json == null || age !in 0..REFRESH_MS) {
            fetch(url)?.let { fresh ->
                if (runCatching { parse(fresh) }.isSuccess) {
                    json = fresh
                    prefs.edit().putString(KEY_JSON, fresh).putLong(KEY_AT, System.currentTimeMillis()).apply()
                }
            }
        }
        return json?.let { runCatching { eligible(parse(it)) }.getOrNull() }.orEmpty()
    }

    private suspend fun fetch(url: String): String? = withContext(Dispatchers.IO) {
        if (!url.startsWith("https://")) return@withContext null
        runCatching {
            val conn = URL(url).openConnection() as HttpURLConnection
            conn.connectTimeout = 5_000
            conn.readTimeout = 5_000
            conn.useCaches = false
            try {
                if (conn.responseCode != 200) return@runCatching null
                val bytes = conn.inputStream.use { it.readNBytesCompat(MAX_BYTES + 1) }
                if (bytes.size > MAX_BYTES) null else String(bytes, Charsets.UTF_8)
            } finally {
                conn.disconnect()
            }
        }.onFailure { Log.w(TAG, "GET $url failed: $it") }.getOrNull()
    }

    private class SponsorFile(val items: List<JSONObject>, val country: String?)

    /** Raw items; throws on a malformed file (the cached copy then stays). */
    private fun parse(text: String): SponsorFile {
        val root = JSONObject(text)
        val items = root.getJSONArray("items")
        val country = root.optString("country").takeIf { Regex("^[A-Z]{2}$").matches(it) }
        return SponsorFile((0 until items.length()).map { items.getJSONObject(it) }, country)
    }

    private fun eligible(file: SponsorFile): List<Sponsor> {
        val items = file.items
        val lang = appLanguage(Locale.getDefault())
        val region = file.country ?: Locale.getDefault().country.uppercase(Locale.ROOT)
        val today = SimpleDateFormat("yyyy-MM-dd", Locale.ROOT).apply { timeZone = TimeZone.getTimeZone("UTC") }.format(Date())
        return items.mapNotNull { o -> runCatching { toSponsor(o, lang, region, today) }.getOrNull() }
    }

    private fun toSponsor(o: JSONObject, lang: String, region: String, today: String): Sponsor? {
        val id = o.optString("id").takeIf { ID.matches(it) } ?: return null
        val kind = o.optString("kind")
        if (kind != "house" && kind != "sponsored") return null
        o.optJSONArray("languages")?.let { a -> if ((0 until a.length()).none { a.getString(it) == lang }) return null }
        o.optJSONArray("regions")?.let { a -> if ((0 until a.length()).none { a.getString(it).equals(region, true) }) return null }
        o.optJSONArray("excludeRegions")?.let { a -> if ((0 until a.length()).any { a.getString(it).equals(region, true) }) return null }
        o.optString("start").takeIf { it.isNotBlank() }?.let { if (today < it) return null }
        o.optString("end").takeIf { it.isNotBlank() }?.let { if (today > it) return null }
        val title = localized(o.optJSONObject("title"), lang)?.take(MAX_TITLE) ?: return null
        val weight = o.optInt("weight", 1).coerceAtLeast(0)
        if (weight == 0) return null
        return Sponsor(
            id = id,
            house = kind == "house",
            title = title,
            body = localized(o.optJSONObject("body"), lang)?.take(MAX_BODY),
            qr = "$SITE/c/$id".takeIf { o.optString("qr").let { q -> q.startsWith("https://") && q.length <= 300 } },
            image = o.optString("image").takeIf { it.startsWith("https://") },
            weight = weight,
        )
    }

    private fun localized(map: JSONObject?, lang: String): String? {
        map ?: return null
        val base = lang.substringBefore('-')
        return listOf(lang, base, "en").firstNotNullOfOrNull { k -> map.optString(k).takeIf { it.isNotBlank() } }
            ?: map.keys().asSequence().map { map.optString(it) }.firstOrNull { it.isNotBlank() }
    }

    /** Our language tags, as the Vega app picks them (i18n.ts pickLanguage). */
    fun appLanguage(locale: Locale): String {
        val lang = locale.language.lowercase(Locale.ROOT)
        return when (lang) {
            "zh" -> {
                val traditional = locale.script.equals("Hant", true) || locale.country.uppercase(Locale.ROOT) in setOf("TW", "HK", "MO")
                if (traditional) "zh-TW" else "zh-CN"
            }
            "pt" -> "pt-BR"
            in setOf("en", "ja", "ko", "es", "fr", "de", "it") -> lang
            else -> "en"
        }
    }
}

/** InputStream.readNBytes is API 33+. */
private fun java.io.InputStream.readNBytesCompat(limit: Int): ByteArray {
    val out = java.io.ByteArrayOutputStream()
    val buf = ByteArray(8192)
    while (out.size() < limit) {
        val n = read(buf, 0, minOf(buf.size, limit - out.size()))
        if (n < 0) break
        out.write(buf, 0, n)
    }
    return out.toByteArray()
}
