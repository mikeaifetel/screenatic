import {ProbeNative} from '@amazon-devices/probenative';

/**
 * Events from the native receivers (AirPlayTest::pollEvents): AirPlay, and DLNA with
 * source "dlna" (DlnaRenderer). Events without a source are AirPlay's.
 */
export type Source = 'airplay' | 'dlna';
export type MediaKind = 'video' | 'audio' | 'image';
export type PlayEvent = {
  type: 'play';
  url: string;
  startMs: number;
  gain: number;
  source?: Source;
  /** DLNA: what the URL is (AirPlay URL mode is always video). */
  mediaType?: MediaKind;
  title?: string;
  artist?: string;
  album?: string;
  artworkUrl?: string;
  /** DLNA: the controller ("Android", "Windows PC", "BubbleUPnP" or its address). */
  client?: string;
};
export type AirPlayEvent =
  | PlayEvent
  | {type: 'cmd'; cmd: number; value: number; source?: Source}
  | {type: 'volume'; gain: number; source?: Source}
  | {type: 'size'; width: number; height: number}
  | {type: 'disconnected'}
  | {type: 'mirror'};

type Listener = (ev: AirPlayEvent) => void;
const listeners = new Set<Listener>();
let timer: ReturnType<typeof setInterval> | null = null;

/** One poller for the native event queue (it is drained by each read), shared by all screens. */
export function onAirPlayEvent(fn: Listener): () => void {
  listeners.add(fn);
  if (!timer) {
    timer = setInterval(() => {
      let events: AirPlayEvent[] = [];
      try {
        events = JSON.parse(ProbeNative.pollEvents());
      } catch {
        // ignore
      }
      for (const ev of events) listeners.forEach((l) => l(ev));
    }, 200);
  }
  return () => {
    listeners.delete(fn);
    if (listeners.size === 0 && timer) {
      clearInterval(timer);
      timer = null;
    }
  };
}
