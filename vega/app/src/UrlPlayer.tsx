import React, {useEffect, useRef, useState} from 'react';
import {StyleSheet, Text, View} from 'react-native';
import {KeplerVideoSurfaceView, VideoPlayer} from '@amazon-devices/react-native-w3cmedia';
import {ProbeNative} from '@amazon-devices/probenative';

type PlayEvent = {type: 'play'; url: string; startMs: number; gain: number};
type CmdEvent = {type: 'cmd'; cmd: number; value: number};
type AirPlayEvent = PlayEvent | CmdEvent | {type: 'volume'; gain: number} | {type: 'disconnected'};

/**
 * AirPlay video (URL mode): apps like YouTube or Safari hand over a media URL instead of
 * mirroring, and the TV fetches the video itself. Vega's player streams it natively (no MSE, so
 * none of the mirror's buffer limits). Its own player and surface, created per video and torn
 * down when the sender stops, so the mirror player is never touched.
 */
export const UrlPlayer = () => {
  const [active, setActive] = useState(false);
  const [state, setState] = useState('no AirPlay video');
  const player = useRef<VideoPlayer | null>(null);
  const surface = useRef<string | null>(null);

  useEffect(() => {
    let reporter: ReturnType<typeof setInterval> | null = null;
    let finished = false;

    const stop = (why: string) => {
      if (reporter) clearInterval(reporter);
      reporter = null;
      const vp = player.current;
      player.current = null;
      if (vp) {
        try {
          vp.pause();
          if (surface.current) vp.clearSurfaceHandle(surface.current);
          vp.deinitialize?.();
        } catch {}
      }
      ProbeNative.setPlayback('0,0,0,0,0,0');
      setActive(false);
      setState(why);
    };

    const start = (ev: PlayEvent) => {
      stop('switching video');
      finished = false;
      setActive(true);
      setState(`loading ${ev.url.slice(0, 120)}`);
      console.log(`UrlPlayer: play ${ev.url}`);
      const vp = new VideoPlayer();
      player.current = vp;
      const report = (s: string) => {
        setState(s);
        ProbeNative.report('url', JSON.stringify({url: ev.url.slice(0, 200), state: s, t: vp.currentTime, rs: vp.readyState}));
      };
      vp.addEventListener('error', () => {
        const err = (vp as any).error;
        report(`player error: code ${err?.code ?? '?'} ${err?.message ?? ''}`);
      });
      vp.addEventListener('loadedmetadata', () => report(`metadata: duration ${vp.duration}`));
      vp.addEventListener('ended', () => {
        finished = true;
        setState('ended');
      });
      vp.addEventListener('playing', () => report(`playing ${ev.url.slice(0, 120)}`));
      vp.initialize()
        .then(() => {
          if (player.current !== vp) return;
          if (surface.current) vp.setSurfaceHandle(surface.current);
          vp.autoplay = true;
          vp.volume = ev.gain;  // the TV's saved AirPlay volume, as for mirrored audio
          vp.src = ev.url;
          if (ev.startMs > 0) {
            const seek = () => {
              vp.currentTime = ev.startMs / 1000;
              vp.removeEventListener('loadedmetadata', seek);
            };
            vp.addEventListener('loadedmetadata', seek);
          }
          vp.play();
        })
        .catch((e: unknown) => setState(`player init failed: ${String(e)}`));

      // The sender polls /playback-info about once a second; keep its view current.
      reporter = setInterval(() => {
        const p = player.current;
        if (!p) return;
        const dur = Number.isFinite(p.duration) ? p.duration : 0;
        const ready = p.readyState >= 2;
        const buffering = !p.paused && p.readyState < 3;
        ProbeNative.setPlayback(
          [dur, p.currentTime, p.paused ? 0 : 1, ready ? 1 : 0, buffering ? 1 : 0, finished ? 1 : 0].join(','),
        );
      }, 500);
    };

    const poll = setInterval(() => {
      let events: AirPlayEvent[] = [];
      try {
        events = JSON.parse(ProbeNative.pollEvents());
      } catch {}
      for (const ev of events) {
        if (ev.type === 'play') {
          start(ev);
        } else if (ev.type === 'cmd') {
          const vp = player.current;
          if (!vp) continue;
          if (ev.cmd === 0) vp.currentTime = ev.value;
          else if (ev.cmd === 1) ev.value > 0 ? vp.play() : vp.pause();
          else if (ev.cmd === 2) stop('stopped by sender');
        } else if (ev.type === 'volume') {
          if (player.current) player.current.volume = ev.gain;
        } else if (ev.type === 'disconnected' && player.current) {
          stop('sender disconnected');
        }
      }
    }, 200);

    return () => {
      clearInterval(poll);
      stop('unmounted');
    };
  }, []);

  return (
    <View style={active ? styles.overlay : undefined}>
      {active ? (
        <View style={styles.surface}>
          <KeplerVideoSurfaceView
            style={styles.fill}
            onSurfaceViewCreated={(h: string) => {
              surface.current = h;
              player.current?.setSurfaceHandle(h);
            }}
            onSurfaceViewDestroyed={(h: string) => {
              player.current?.clearSurfaceHandle(h);
              surface.current = null;
            }}
          />
        </View>
      ) : null}
      <Text style={styles.info}>AirPlay video: {state}</Text>
    </View>
  );
};

const styles = StyleSheet.create({
  info: {color: '#9fe39f', fontSize: 10},
  // While a video plays, cover the mirror's picture at the top of the right panel (the two never
  // play at once); otherwise just a status line below the mirror.
  overlay: {position: 'absolute', top: 10, left: 10, right: 10, backgroundColor: '#182028'},
  surface: {width: '100%', aspectRatio: 16 / 9, backgroundColor: '#000'},
  fill: {flex: 1},
});
