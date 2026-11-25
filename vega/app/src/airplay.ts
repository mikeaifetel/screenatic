import {ProbeNative} from '@amazon-devices/probenative';

/** Events from the native AirPlay server (AirPlayTest::pollEvents). */
export type PlayEvent = {
  type: 'play';
  url: string;
  startMs: number;
  gain: number;
};
export type AirPlayEvent =
  | PlayEvent
  | {type: 'cmd'; cmd: number; value: number}
  | {type: 'volume'; gain: number}
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
