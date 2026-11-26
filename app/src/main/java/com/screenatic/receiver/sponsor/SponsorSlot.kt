package com.screenatic.receiver.sponsor

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.util.LruCache
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.requiredSize
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.FilterQuality
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.tv.material3.MaterialTheme
import androidx.tv.material3.Text
import com.google.zxing.BarcodeFormat
import com.google.zxing.EncodeHintType
import com.google.zxing.qrcode.QRCodeWriter
import com.screenatic.receiver.R
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import java.net.HttpURLConnection
import java.net.URL
import kotlin.random.Random

/** Each item stays on screen at least this long (the sponsor-slot plan: 30 s). */
private const val ROTATE_MS = 30_000L
private const val MAX_IMAGE_BYTES = 1024 * 1024

/**
 * The idle-screen sponsor slot: label ("Sponsored" / "From Screenatic"), optional picture, title,
 * text and a QR code to scan with a phone. Not focusable and not clickable: Google TV forbids ads
 * that open a web page (TV-AU, TV-WB), so the QR code is the only way out.
 */
@Composable
fun SponsorSlot(items: List<Sponsor>, onShown: (String) -> Unit, modifier: Modifier = Modifier) {
    if (items.isEmpty()) return
    var current by remember(items) { mutableStateOf(pick(items, null)) }
    var turn by remember(items) { mutableStateOf(0) }
    LaunchedEffect(items) {
        while (true) {
            delay(ROTATE_MS)
            current = pick(items, current)
            turn++
        }
    }
    val s = current
    // An impression: the item stayed on screen for a second (IAB/MRC display viewability).
    LaunchedEffect(s.id, turn) {
        delay(1_000)
        onShown(s.id)
    }
    Column(
        modifier
            .clip(RoundedCornerShape(24.dp))
            .background(MaterialTheme.colorScheme.surface)
            .padding(28.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        Text(
            stringResource(if (s.house) R.string.house_label else R.string.sponsored_label),
            style = MaterialTheme.typography.labelLarge,
            color = Color.White,
            modifier = Modifier
                .clip(RoundedCornerShape(8.dp))
                .background(Color(0x66FFFFFF).takeIf { s.house } ?: MaterialTheme.colorScheme.primary)
                .padding(horizontal = 10.dp, vertical = 4.dp),
        )
        s.image?.let { url ->
            val bitmap by produceState<ImageBitmap?>(null, url) { value = loadImage(url) }
            bitmap?.let {
                Image(it, null, Modifier.fillMaxWidth().heightIn(max = 96.dp), contentScale = ContentScale.Fit, alignment = Alignment.CenterStart)
            }
        }
        Text(s.title, style = MaterialTheme.typography.headlineSmall, maxLines = 2, overflow = TextOverflow.Ellipsis)
        s.body?.let {
            Text(it, style = MaterialTheme.typography.bodyMedium, maxLines = 4, overflow = TextOverflow.Ellipsis,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Spacer(Modifier.weight(1f, fill = true))
        s.qr?.let { url ->
            val qr = remember(url) { qrBitmap(url) }
            if (qr != null) Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                Box(Modifier.clip(RoundedCornerShape(8.dp)).background(Color.White).padding(8.dp)) {
                    Image(qr, null, Modifier.requiredSize(132.dp), filterQuality = FilterQuality.None)
                }
                Text(stringResource(R.string.scan_qr), style = MaterialTheme.typography.bodyMedium)
            }
        }
    }
}

/** Weighted random pick, avoiding the one on screen when there's a choice. */
private fun pick(items: List<Sponsor>, previous: Sponsor?): Sponsor {
    val pool = items.filter { it.id != previous?.id }.ifEmpty { items }
    var r = Random.nextInt(pool.sumOf { it.weight })
    for (s in pool) {
        r -= s.weight
        if (r < 0) return s
    }
    return pool.last()
}

private val qrCache = LruCache<String, ImageBitmap>(8)

private fun qrBitmap(text: String): ImageBitmap? = qrCache.get(text) ?: runCatching {
    val m = QRCodeWriter().encode(text, BarcodeFormat.QR_CODE, 0, 0, mapOf(EncodeHintType.MARGIN to 0))
    val px = IntArray(m.width * m.height) { i -> if (m.get(i % m.width, i / m.width)) 0xFF000000.toInt() else 0xFFFFFFFF.toInt() }
    Bitmap.createBitmap(px, m.width, m.height, Bitmap.Config.ARGB_8888).asImageBitmap()
}.getOrNull()?.also { qrCache.put(text, it) }

private val imageCache = LruCache<String, ImageBitmap>(8)

private suspend fun loadImage(url: String): ImageBitmap? = imageCache.get(url) ?: withContext(Dispatchers.IO) {
    runCatching {
        val conn = URL(url).openConnection() as HttpURLConnection
        conn.connectTimeout = 5_000
        conn.readTimeout = 10_000
        try {
            if (conn.responseCode != 200 || conn.contentLengthLong > MAX_IMAGE_BYTES) return@runCatching null
            val bytes = conn.inputStream.use { it.readBytes() }
            if (bytes.size > MAX_IMAGE_BYTES) return@runCatching null
            BitmapFactory.decodeByteArray(bytes, 0, bytes.size)?.asImageBitmap()
        } finally {
            conn.disconnect()
        }
    }.getOrNull()
}?.also { imageCache.put(url, it) }
