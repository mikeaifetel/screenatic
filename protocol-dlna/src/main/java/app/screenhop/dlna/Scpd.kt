package app.screenhop.dlna

/**
 * Service descriptions (SCPD) for the three MediaRenderer services.
 * Actions are written as "in:Arg=StateVar" / "out:Arg=StateVar" to keep this readable.
 */
internal object Scpd {

    private class Var(val name: String, val type: String = "string", val events: Boolean = false, val allowed: List<String> = emptyList())

    private fun build(actions: List<Pair<String, List<String>>>, vars: List<Var>): String = buildString {
        append("""<?xml version="1.0" encoding="utf-8"?><scpd xmlns="urn:schemas-upnp-org:service-1-0">""")
        append("<specVersion><major>1</major><minor>0</minor></specVersion><actionList>")
        for ((name, args) in actions) {
            append("<action><name>$name</name>")
            if (args.isNotEmpty()) {
                append("<argumentList>")
                for (a in args) {
                    val dir = a.substringBefore(':')
                    val arg = a.substringAfter(':').substringBefore('=')
                    val v = a.substringAfter('=')
                    append("<argument><name>$arg</name><direction>$dir</direction><relatedStateVariable>$v</relatedStateVariable></argument>")
                }
                append("</argumentList>")
            }
            append("</action>")
        }
        append("</actionList><serviceStateTable>")
        for (v in vars) {
            append("<stateVariable sendEvents=\"${if (v.events) "yes" else "no"}\"><name>${v.name}</name><dataType>${v.type}</dataType>")
            if (v.name == "Volume") append("<allowedValueRange><minimum>0</minimum><maximum>100</maximum><step>1</step></allowedValueRange>")
            if (v.allowed.isNotEmpty()) {
                append("<allowedValueList>")
                v.allowed.forEach { append("<allowedValue>$it</allowedValue>") }
                append("</allowedValueList>")
            }
            append("</stateVariable>")
        }
        append("</serviceStateTable></scpd>")
    }

    private const val ID = "in:InstanceID=A_ARG_TYPE_InstanceID"

    val avTransport: String = build(
        listOf(
            "SetAVTransportURI" to listOf(ID, "in:CurrentURI=AVTransportURI", "in:CurrentURIMetaData=AVTransportURIMetaData"),
            "SetNextAVTransportURI" to listOf(ID, "in:NextURI=NextAVTransportURI", "in:NextURIMetaData=NextAVTransportURIMetaData"),
            "GetMediaInfo" to listOf(
                ID, "out:NrTracks=NumberOfTracks", "out:MediaDuration=CurrentMediaDuration", "out:CurrentURI=AVTransportURI",
                "out:CurrentURIMetaData=AVTransportURIMetaData", "out:NextURI=NextAVTransportURI",
                "out:NextURIMetaData=NextAVTransportURIMetaData", "out:PlayMedium=PlaybackStorageMedium",
                "out:RecordMedium=RecordStorageMedium", "out:WriteStatus=RecordMediumWriteStatus",
            ),
            "GetTransportInfo" to listOf(
                ID, "out:CurrentTransportState=TransportState", "out:CurrentTransportStatus=TransportStatus",
                "out:CurrentSpeed=TransportPlaySpeed",
            ),
            "GetPositionInfo" to listOf(
                ID, "out:Track=CurrentTrack", "out:TrackDuration=CurrentTrackDuration", "out:TrackMetaData=CurrentTrackMetaData",
                "out:TrackURI=CurrentTrackURI", "out:RelTime=RelativeTimePosition", "out:AbsTime=AbsoluteTimePosition",
                "out:RelCount=RelativeCounterPosition", "out:AbsCount=AbsoluteCounterPosition",
            ),
            "GetDeviceCapabilities" to listOf(
                ID, "out:PlayMedia=PossiblePlaybackStorageMedia", "out:RecMedia=PossibleRecordStorageMedia",
                "out:RecQualityModes=PossibleRecordQualityModes",
            ),
            "GetTransportSettings" to listOf(ID, "out:PlayMode=CurrentPlayMode", "out:RecQualityMode=CurrentRecordQualityMode"),
            "Stop" to listOf(ID),
            "Play" to listOf(ID, "in:Speed=TransportPlaySpeed"),
            "Pause" to listOf(ID),
            "Seek" to listOf(ID, "in:Unit=A_ARG_TYPE_SeekMode", "in:Target=A_ARG_TYPE_SeekTarget"),
            "Next" to listOf(ID),
            "Previous" to listOf(ID),
            "GetCurrentTransportActions" to listOf(ID, "out:Actions=CurrentTransportActions"),
        ),
        listOf(
            Var("TransportState", allowed = listOf("STOPPED", "PLAYING", "PAUSED_PLAYBACK", "TRANSITIONING", "NO_MEDIA_PRESENT")),
            Var("TransportStatus", allowed = listOf("OK", "ERROR_OCCURRED")),
            Var("PlaybackStorageMedium", allowed = listOf("NONE", "NETWORK")),
            Var("RecordStorageMedium", allowed = listOf("NOT_IMPLEMENTED")),
            Var("PossiblePlaybackStorageMedia"), Var("PossibleRecordStorageMedia"),
            Var("CurrentPlayMode", allowed = listOf("NORMAL")),
            Var("TransportPlaySpeed", allowed = listOf("1")),
            Var("RecordMediumWriteStatus", allowed = listOf("NOT_IMPLEMENTED")),
            Var("CurrentRecordQualityMode", allowed = listOf("NOT_IMPLEMENTED")),
            Var("PossibleRecordQualityModes"),
            Var("NumberOfTracks", "ui4"), Var("CurrentTrack", "ui4"),
            Var("CurrentTrackDuration"), Var("CurrentMediaDuration"), Var("CurrentTrackMetaData"), Var("CurrentTrackURI"),
            Var("AVTransportURI"), Var("AVTransportURIMetaData"), Var("NextAVTransportURI"), Var("NextAVTransportURIMetaData"),
            Var("RelativeTimePosition"), Var("AbsoluteTimePosition"),
            Var("RelativeCounterPosition", "i4"), Var("AbsoluteCounterPosition", "i4"),
            Var("CurrentTransportActions"),
            Var("LastChange", events = true),
            Var("A_ARG_TYPE_SeekMode", allowed = listOf("REL_TIME", "ABS_TIME", "TRACK_NR")),
            Var("A_ARG_TYPE_SeekTarget"),
            Var("A_ARG_TYPE_InstanceID", "ui4"),
        ),
    )

    private const val CH = "in:Channel=A_ARG_TYPE_Channel"

    val renderingControl: String = build(
        listOf(
            "ListPresets" to listOf(ID, "out:CurrentPresetNameList=PresetNameList"),
            "SelectPreset" to listOf(ID, "in:PresetName=A_ARG_TYPE_PresetName"),
            "GetMute" to listOf(ID, CH, "out:CurrentMute=Mute"),
            "SetMute" to listOf(ID, CH, "in:DesiredMute=Mute"),
            "GetVolume" to listOf(ID, CH, "out:CurrentVolume=Volume"),
            "SetVolume" to listOf(ID, CH, "in:DesiredVolume=Volume"),
        ),
        listOf(
            Var("LastChange", events = true),
            Var("PresetNameList"),
            Var("Mute", "boolean"),
            Var("Volume", "ui2"),
            Var("A_ARG_TYPE_Channel", allowed = listOf("Master")),
            Var("A_ARG_TYPE_InstanceID", "ui4"),
            Var("A_ARG_TYPE_PresetName", allowed = listOf("FactoryDefaults")),
        ),
    )

    val connectionManager: String = build(
        listOf(
            "GetProtocolInfo" to listOf("out:Source=SourceProtocolInfo", "out:Sink=SinkProtocolInfo"),
            "GetCurrentConnectionIDs" to listOf("out:ConnectionIDs=CurrentConnectionIDs"),
            "GetCurrentConnectionInfo" to listOf(
                "in:ConnectionID=A_ARG_TYPE_ConnectionID", "out:RcsID=A_ARG_TYPE_RcsID",
                "out:AVTransportID=A_ARG_TYPE_AVTransportID", "out:ProtocolInfo=A_ARG_TYPE_ProtocolInfo",
                "out:PeerConnectionManager=A_ARG_TYPE_ConnectionManager", "out:PeerConnectionID=A_ARG_TYPE_ConnectionID",
                "out:Direction=A_ARG_TYPE_Direction", "out:Status=A_ARG_TYPE_ConnectionStatus",
            ),
        ),
        listOf(
            Var("SourceProtocolInfo", events = true), Var("SinkProtocolInfo", events = true),
            Var("CurrentConnectionIDs", events = true),
            Var("A_ARG_TYPE_ConnectionStatus", allowed = listOf("OK", "ContentFormatMismatch", "InsufficientBandwidth", "UnreliableChannel", "Unknown")),
            Var("A_ARG_TYPE_ConnectionManager"),
            Var("A_ARG_TYPE_Direction", allowed = listOf("Input", "Output")),
            Var("A_ARG_TYPE_ProtocolInfo"),
            Var("A_ARG_TYPE_ConnectionID", "i4"), Var("A_ARG_TYPE_AVTransportID", "i4"), Var("A_ARG_TYPE_RcsID", "i4"),
        ),
    )

    fun forPath(path: String): String? = when (path) {
        "AVTransport" -> avTransport
        "RenderingControl" -> renderingControl
        "ConnectionManager" -> connectionManager
        else -> null
    }
}
