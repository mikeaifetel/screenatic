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
  /** Drains up to maxBytes of the mirror's audio (FLAC fMP4, its own stream). */
  readAudio: (maxBytes: number) => ArrayBuffer;
  /** JS player state for remote diagnostics: GET http://<tv>:7000/status returns it under "js". */
  report: (section: string, json: string) => void;
  /** AirPlay video (URL mode) events since the last call, as a JSON array (see AirPlayTest.h). */
  pollEvents: () => string;
  /** "duration,position,rate,ready,buffering,finished" for the sender's progress polls. */
  setPlayback: (csv: string) => void;
  msseAttach: (windowHandle: string) => string;
  /** Starts the real AirPlay server (UxPlay) and advertises it instead of the placeholder. */
  airplayStart: (deviceName: string) => string;
}

export default TurboModuleRegistry.getEnforcing<ProbeNative>('ProbeNative');
