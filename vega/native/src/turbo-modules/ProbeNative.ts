import type {KeplerTurboModule} from '@amazon-devices/keplerscript-turbomodule-api';
import {TurboModuleRegistry} from '@amazon-devices/keplerscript-turbomodule-api';

/**
 * Feasibility probe for a Screenatic receiver on Vega OS. Answers the questions we can't settle
 * from docs: can other LAN devices reach a port we listen on, do our mDNS (AirPlay) and SSDP (DLNA)
 * announcements show up on phones/PCs, and can we stream bytes from a socket into the player.
 */
export interface ProbeNative extends KeplerTurboModule {
  /** Starts listeners and announcements. Returns the status JSON (see status()). */
  start: (deviceName: string) => string;
  /** JSON: {running, ip, ports, mdns, ssdp, video, log: string[]} */
  status: () => string;
  stop: () => void;
  /** Drains up to maxBytes received on the video ingest port (raw fMP4 stream). */
  readVideo: (maxBytes: number) => ArrayBuffer;
}

export default TurboModuleRegistry.getEnforcing<ProbeNative>('ProbeNative');
