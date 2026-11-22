package app.screenatic.receiver.ui

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.tv.material3.Text
import app.screenatic.core.session.ReceiverUiState
import app.screenatic.receiver.R
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.net.HttpURLConnection
import java.net.URL

/** Photo pushed by a DLNA controller (gallery apps, Windows "Cast to Device"). */
@Composable
fun ImageViewer(url: String, ui: ReceiverUiState, onWatchAd: () -> Unit) {
    val result by produceState<Result<ImageBitmap>?>(null, url) {
        value = runCatching { loadBitmap(url, 1920, 1080)!!.asImageBitmap() }
    }
    Box(Modifier.fillMaxSize().background(Color.Black), contentAlignment = Alignment.Center) {
        val r = result
        when {
            r == null -> Text(stringResource(R.string.image_loading), color = Color.Gray)
            r.isSuccess -> Image(r.getOrThrow(), contentDescription = null, Modifier.fillMaxSize(), contentScale = ContentScale.Fit)
            else -> Text(stringResource(R.string.image_failed), color = Color.Gray)
        }
        GraceBanner(ui, onWatchAd, Modifier.align(Alignment.BottomCenter).padding(24.dp))
    }
}

/** Download and decode an image, subsampled to roughly fit [maxW]x[maxH] (camera photos are huge). */
suspend fun loadBitmap(url: String, maxW: Int, maxH: Int): Bitmap? = withContext(Dispatchers.IO) {
    val bytes = (URL(url).openConnection() as HttpURLConnection).run {
        connectTimeout = 10_000
        readTimeout = 20_000
        try { inputStream.use { it.readBytes() } } finally { disconnect() }
    }
    val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
    var sample = 1
    while (bounds.outWidth / (sample * 2) >= maxW || bounds.outHeight / (sample * 2) >= maxH) sample *= 2
    BitmapFactory.decodeByteArray(bytes, 0, bytes.size, BitmapFactory.Options().apply { inSampleSize = sample })
}
