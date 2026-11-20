package app.screenhop.dlna

import android.content.Context
import app.screenhop.core.receiver.EngineState
import app.screenhop.core.receiver.Protocol
import app.screenhop.core.receiver.ReceiverConfig
import app.screenhop.core.receiver.ReceiverEngine
import app.screenhop.core.receiver.SessionKind
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * DLNA / UPnP MediaRenderer (DMR).
 *
 * TODO (M1):
 *  1. SSDP: announce `urn:schemas-upnp-org:device:MediaRenderer:1` on 239.255.255.250:1900,
 *     answer M-SEARCH.
 *  2. HTTP server: device description XML + SCPD for AVTransport, RenderingControl,
 *     ConnectionManager.
 *  3. SOAP actions: SetAVTransportURI -> [onSetUri]; Play/Pause/Stop/Seek;
 *     GetPositionInfo / GetTransportInfo from the player; SetVolume.
 *  4. GENA eventing (LastChange) so phone apps show progress.
 * Option: hand-roll (small, no deps) vs. jUPnP. Hand-rolled keeps the GPL/APK footprint small.
 */
class DlnaEngine(@Suppress("unused") private val context: Context) : ReceiverEngine {

    override val protocol = Protocol.DLNA

    private val _state = MutableStateFlow<EngineState>(EngineState.Stopped)
    override val state: StateFlow<EngineState> = _state.asStateFlow()

    override fun start(config: ReceiverConfig) {
        // TODO: start SSDP + HTTP/SOAP server.
        _state.value = EngineState.Advertising
    }

    override fun stop() {
        _state.value = EngineState.Stopped
    }

    override fun disconnectClient() {
        if (_state.value is EngineState.Connected) _state.value = EngineState.Advertising
    }

    /** SOAP SetAVTransportURI + Play end up here. */
    internal fun onSetUri(controllerName: String, uri: String) {
        _state.value = EngineState.Connected(controllerName, SessionKind.MEDIA_URL, uri)
    }
}
