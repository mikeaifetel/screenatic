package com.screenatic.dlna

import android.util.Xml
import com.screenatic.core.receiver.MediaType
import org.xmlpull.v1.XmlPullParser
import java.io.StringReader

/** UPnP / DLNA constants and XML builders. Hand-rolled: three services don't justify a UPnP stack. */
internal object Upnp {
    const val DEVICE_TYPE = "urn:schemas-upnp-org:device:MediaRenderer:1"
    const val AVT = "urn:schemas-upnp-org:service:AVTransport:1"
    const val RCS = "urn:schemas-upnp-org:service:RenderingControl:1"
    const val CMS = "urn:schemas-upnp-org:service:ConnectionManager:1"

    /** Path segment per service: /AVTransport/control, /AVTransport/event, /AVTransport/scpd.xml */
    val services = linkedMapOf(AVT to "AVTransport", RCS to "RenderingControl", CMS to "ConnectionManager")

    /** What we can play (ExoPlayer formats + images). Phones filter their "cast to" list with this. */
    val sinkProtocolInfo = listOf(
        "video/mp4", "video/x-matroska", "video/webm", "video/mpeg", "video/mp2t", "video/quicktime",
        "video/x-msvideo", "video/3gpp", "video/x-flv", "application/vnd.apple.mpegurl", "application/x-mpegURL",
        "application/dash+xml", "audio/mpeg", "audio/mp4", "audio/aac", "audio/x-aac", "audio/flac", "audio/x-flac",
        "audio/wav", "audio/x-wav", "audio/L16", "audio/ogg", "audio/opus", "audio/webm", "audio/x-ms-wma",
        "image/jpeg", "image/png", "image/gif", "image/webp", "image/bmp", "image/heic",
    ).joinToString(",") { "http-get:*:$it:*" }

    fun deviceDescription(friendlyName: String, uuid: String): String = buildString {
        append("""<?xml version="1.0" encoding="utf-8"?>""")
        append("""<root xmlns="urn:schemas-upnp-org:device-1-0" xmlns:dlna="urn:schemas-dlna-org:device-1-0">""")
        append("<specVersion><major>1</major><minor>0</minor></specVersion><device>")
        append("<deviceType>$DEVICE_TYPE</deviceType>")
        append("<friendlyName>${esc(friendlyName)}</friendlyName>")
        append("<manufacturer>Screenatic</manufacturer>")
        append("<modelDescription>Screenatic TV receiver</modelDescription><modelName>Screenatic</modelName>")
        append("<modelNumber>1</modelNumber><UDN>uuid:$uuid</UDN>")
        append("<dlna:X_DLNADOC>DMR-1.50</dlna:X_DLNADOC>")
        append("<iconList><icon><mimetype>image/png</mimetype><width>192</width><height>192</height><depth>32</depth>")
        append("<url>/icon.png</url></icon></iconList><serviceList>")
        for ((type, path) in services) {
            append("<service><serviceType>$type</serviceType><serviceId>urn:upnp-org:serviceId:$path</serviceId>")
            append("<SCPDURL>/$path/scpd.xml</SCPDURL><controlURL>/$path/control</controlURL>")
            append("<eventSubURL>/$path/event</eventSubURL></service>")
        }
        append("</serviceList></device></root>")
    }

    fun soapResponse(serviceType: String, action: String, out: List<Pair<String, String>>): String = buildString {
        append("""<?xml version="1.0" encoding="utf-8"?>""")
        append("""<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>""")
        append("<u:${action}Response xmlns:u=\"$serviceType\">")
        for ((k, v) in out) append("<$k>${esc(v)}</$k>")
        append("</u:${action}Response></s:Body></s:Envelope>")
    }

    fun soapFault(code: Int, description: String): String =
        """<?xml version="1.0" encoding="utf-8"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body><s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail><UPnPError xmlns="urn:schemas-upnp-org:control-1-0"><errorCode>$code</errorCode><errorDescription>${esc(description)}</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>"""

    /** GENA body carrying one LastChange (itself an escaped XML document). */
    fun lastChangeEvent(serviceType: String, vars: List<Pair<String, String>>): String {
        val ns = if (serviceType == RCS) "urn:schemas-upnp-org:metadata-1-0/RCS/" else "urn:schemas-upnp-org:metadata-1-0/AVT/"
        val inner = buildString {
            append("<Event xmlns=\"$ns\"><InstanceID val=\"0\">")
            for ((k, v) in vars) {
                if (serviceType == RCS) append("<$k channel=\"Master\" val=\"${esc(v)}\"/>")
                else append("<$k val=\"${esc(v)}\"/>")
            }
            append("</InstanceID></Event>")
        }
        return """<?xml version="1.0" encoding="utf-8"?><e:propertyset xmlns:e="urn:schemas-upnp-org:event-1-0"><e:property><LastChange>${esc(inner)}</LastChange></e:property></e:propertyset>"""
    }

    fun esc(s: String): String = buildString(s.length) {
        for (c in s) when (c) {
            '&' -> append("&amp;"); '<' -> append("&lt;"); '>' -> append("&gt;")
            '"' -> append("&quot;"); '\'' -> append("&apos;")
            else -> append(c)
        }
    }

    /** UPnP time "H:MM:SS". */
    fun formatTime(ms: Long): String {
        val s = (ms.coerceAtLeast(0)) / 1000
        return "%d:%02d:%02d".format(s / 3600, (s / 60) % 60, s % 60)
    }

    /** "H:MM:SS[.fff]" or "MM:SS" or plain seconds -> ms; null if unparseable. */
    fun parseTime(t: String): Long? {
        val parts = t.trim().split(':')
        if (parts.isEmpty() || parts.size > 3) return null
        var sec = 0.0
        for (p in parts) sec = sec * 60 + (p.toDoubleOrNull() ?: return null)
        return (sec * 1000).toLong()
    }
}

/** A SOAP request: action name and its (flat) arguments. */
internal data class SoapCall(val action: String, val args: Map<String, String>)

internal fun parseSoap(body: String): SoapCall? = runCatching {
    val p = Xml.newPullParser()
    p.setFeature(XmlPullParser.FEATURE_PROCESS_NAMESPACES, true)
    p.setInput(StringReader(body))
    var inBody = false
    var action: String? = null
    val args = HashMap<String, String>()
    var depthAction = -1
    while (p.next() != XmlPullParser.END_DOCUMENT) {
        if (p.eventType != XmlPullParser.START_TAG) continue
        when {
            p.name == "Body" -> inBody = true
            inBody && action == null -> { action = p.name; depthAction = p.depth }
            action != null && p.depth == depthAction + 1 -> args[p.name] = p.nextText()
        }
    }
    action?.let { SoapCall(it, args) }
}.getOrNull()

/** What a DIDL-Lite item says about the media. */
internal data class DidlInfo(
    val title: String = "",
    val artist: String = "",
    val album: String = "",
    val artworkUrl: String? = null,
    val mediaType: MediaType? = null,
    val durationMs: Long = 0,
)

internal fun parseDidl(didl: String): DidlInfo {
    if (didl.isBlank()) return DidlInfo()
    return runCatching {
        val p = Xml.newPullParser()
        p.setFeature(XmlPullParser.FEATURE_PROCESS_NAMESPACES, true)
        p.setInput(StringReader(didl))
        var title = ""; var artist = ""; var creator = ""; var album = ""; var art: String? = null
        var upnpClass = ""; var mime = ""; var duration = 0L
        while (p.next() != XmlPullParser.END_DOCUMENT) {
            if (p.eventType != XmlPullParser.START_TAG) continue
            when (p.name) {
                "title" -> title = p.nextText().trim()
                "artist" -> if (artist.isEmpty()) artist = p.nextText().trim()
                "creator" -> creator = p.nextText().trim()
                "album" -> album = p.nextText().trim()
                "albumArtURI" -> if (art == null) art = p.nextText().trim().ifEmpty { null }
                "class" -> upnpClass = p.nextText().trim()
                "res" -> {
                    if (mime.isEmpty()) mime = p.getAttributeValue(null, "protocolInfo")?.split(':')?.getOrNull(2).orEmpty()
                    if (duration == 0L) duration = p.getAttributeValue(null, "duration")?.let(Upnp::parseTime) ?: 0L
                }
            }
        }
        val type = when {
            "imageItem" in upnpClass || mime.startsWith("image/") -> MediaType.IMAGE
            "audioItem" in upnpClass || mime.startsWith("audio/") -> MediaType.AUDIO
            "videoItem" in upnpClass || mime.startsWith("video/") -> MediaType.VIDEO
            else -> null
        }
        DidlInfo(title, artist.ifEmpty { creator }, album, art, type, duration)
    }.getOrDefault(DidlInfo())
}

/** Fallback when the sender gives no metadata: guess from the URL's extension. */
internal fun guessMediaType(url: String): MediaType {
    val ext = url.substringBefore('?').substringAfterLast('.', "").lowercase()
    return when (ext) {
        "jpg", "jpeg", "png", "gif", "webp", "bmp", "heic" -> MediaType.IMAGE
        "mp3", "m4a", "aac", "flac", "wav", "ogg", "opus", "wma" -> MediaType.AUDIO
        else -> MediaType.VIDEO
    }
}
