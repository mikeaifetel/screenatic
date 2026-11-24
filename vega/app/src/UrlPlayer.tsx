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
// Kept across videos so a failed attempt can still be read from :7000/status afterwards.
const history: string[] = [];

export const UrlPlayer = () => {
  const [active, setActive] = useState(false);
  // Until the URL player shows its first frame, its layer stays see-through and the mirror player
  // keeps its last frame on screen (instead of ~2 s of black while the video loads).
  const [shown, setShown] = useState(false);
  const [state, setState] = useState('no AirPlay video');
  const player = useRef<VideoPlayer | null>(null);
  const surface = useRef<string | null>(null);

  useEffect(() => {
    let reporter: ReturnType<typeof setInterval> | null = null;
    let finished = false;
    let startedAt = 0;

    const stop = (why: string) => {
      if (reporter) clearInterval(reporter);
      reporter = null;
      const vp = player.current;
      player.current = null;
      if (vp) {
        // Give the decoder back to the native mirror player once the old player is torn down.
        // (not when stop() is only switching to the next video)
        setTimeout(() => {
          if (!player.current) ProbeNative.msseAttach('cmd:suspend=0');
        }, 150);
      }
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
      if (vp) {
        history.push(`${new Date().toLocaleTimeString('en-GB')} stop: ${why}`);
        while (history.length > 15) history.shift();
        ProbeNative.report('url', JSON.stringify({state: `stopped: ${why}`, history}));
      }
    };

    const start = (ev: PlayEvent) => {
      stop('switching video');
      startedAt = Date.now();
      finished = false;
      // The native mirror player (MSSE) holds the hardware decoder and has to give it up for the
      // URL playback. It keeps showing its last frame until the URL player plays (handOver), or
      // at the latest after 2.5 s in case the URL player needs the decoder to start at all.
      let handedOver = false;
      const handOver = (why: string) => {
        if (handedOver || player.current !== vp) return;
        handedOver = true;
        setShown(true);
        ProbeNative.msseAttach('cmd:suspend=1');
        report(`mirror player released (${why})`);
      };
      setShown(false);
      setActive(true);
      setTimeout(() => handOver('2.5 s timeout'), 2500);
      setState(`loading ${ev.url.slice(0, 120)}`);
      console.log(`UrlPlayer: play ${ev.url}`);
      const vp = new VideoPlayer();
      player.current = vp;
      const report = (s: string) => {
        setState(s);
        history.push(`${new Date().toLocaleTimeString('en-GB')} ${s.slice(0, 160)}`);
        while (history.length > 15) history.shift();
        let t = -1;
        let rs = -1;
        try {
          t = vp.currentTime;
          rs = vp.readyState;
        } catch {}
        ProbeNative.report('url', JSON.stringify({url: ev.url.slice(0, 300), state: s, t, rs, history}));
      };
      report(`received ${ev.url.slice(0, 160)} (start ${ev.startMs} ms)`);
      vp.addEventListener('error', () => {
        const err = (vp as any).error;
        report(`player error: code ${err?.code ?? '?'} ${err?.message ?? ''}`);
      });
      vp.addEventListener('loadedmetadata', () => report(`metadata: duration ${vp.duration}`));
      vp.addEventListener('ended', () => {
        finished = true;
        setState('ended');
      });
      vp.addEventListener('playing', () => {
        report(`playing ${ev.url.slice(0, 120)}`);
        // 'playing' can come before the first frame is on screen: hand over once the position
        // has moved on 0.2 s (at most 1 s later).
        const t0 = vp.currentTime;
        const since = Date.now();
        const wait = setInterval(() => {
          if (player.current !== vp || handedOver) return clearInterval(wait);
          if (vp.currentTime - t0 > 0.2 || Date.now() - since > 1000) {
            clearInterval(wait);
            handOver('URL player showing frames');
          }
        }, 50);
      });
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
          report('initialized, src set, play()');
        })
        .catch((e: unknown) => report(`player init failed: ${String(e)}`));

      // The sender polls /playback-info about once a second; keep its view current.
      let ticks = 0;
      reporter = setInterval(() => {
        const p = player.current;
        if (!p) return;
        if (++ticks % 6 === 0) report(`tick: t=${p.currentTime.toFixed(1)} readyState=${p.readyState} paused=${p.paused}`);
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
          // Handing a video over (Safari fullscreen), the iPhone closes its mirror connection
          // right after sending the URL: that disconnect is not the end of this video.
          if (Date.now() - startedAt < 3000) continue;
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
    <View style={active ? (shown ? styles.overlay : styles.overlayHidden) : undefined}>
      {active ? (
        <View style={shown ? styles.surface : styles.surfaceHidden}>
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
  // Loading: same size (the player needs its surface) but off screen: a native surface view
  // clears its area to black when created, which showed through opacity 0.
  overlayHidden: {position: 'absolute', top: 10, left: -3000, width: 444},
  surfaceHidden: {width: '100%', aspectRatio: 16 / 9},
  fill: {flex: 1},
});
