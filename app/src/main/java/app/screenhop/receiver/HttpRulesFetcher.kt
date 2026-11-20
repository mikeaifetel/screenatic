package app.screenhop.receiver

import app.screenhop.core.session.RulesFetcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import android.util.Log
import java.net.HttpURLConnection
import java.net.URL

/** Fetches the cast-time rules JSON (see CastTimeRulesRepository for the format). */
class HttpRulesFetcher(private val url: String) : RulesFetcher {
    override suspend fun fetch(): String? = withContext(Dispatchers.IO) {
        runCatching {
            val conn = URL(url).openConnection() as HttpURLConnection
            conn.connectTimeout = 5_000
            conn.readTimeout = 5_000
            conn.useCaches = false
            try {
                val code = conn.responseCode
                Log.i(TAG, "GET $url -> $code")
                if (code == 200) conn.inputStream.bufferedReader().use { it.readText() } else null
            } finally {
                conn.disconnect()
            }
        }.onFailure { Log.w(TAG, "GET $url failed: $it") }.getOrNull()
    }
}

private const val TAG = "RulesFetcher"
