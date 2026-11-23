import React, {useEffect, useRef, useState} from 'react';
import {StyleSheet, Text, View} from 'react-native';
import {
  AppendMode,
  KeplerVideoSurfaceView,
  MediaSource,
  SourceBuffer,
  VideoPlayer,
} from '@amazon-devices/react-native-w3cmedia';
import {ProbeNative} from '@amazon-devices/probenative';

// w3cmedia expects browser globals; the docs' headless example sets these before creating the player.
const g = globalThis as any;
if (typeof g.navigator === 'undefined') g.navigator = {};
if (typeof g.window === 'undefined') g.window = g;
if (typeof g.self === 'undefined') g.self = g;

// What a mirroring session will feed: H.264 Baseline/Main in fragmented MP4.
const MIME = 'video/mp4; codecs="avc1.4d401f"';

/**
 * MSE latency test: bytes arriving on TCP :7100 (an fMP4 stream from ffmpeg on the Mac, with the
 * Mac's wall clock burned into the picture) are appended to a SourceBuffer. Comparing the burned-in
 * clock with the TV clock shown below gives glass-to-glass latency.
 */
const Surface = React.memo(({player}: {player: React.MutableRefObject<VideoPlayer | null>}) => (
  <View style={styles.surface}>
    <KeplerVideoSurfaceView
      style={styles.fill}
      onSurfaceViewCreated={(h: string) => player.current?.setSurfaceHandle(h)}
      onSurfaceViewDestroyed={(h: string) => player.current?.clearSurfaceHandle(h)}
    />
  </View>
));

export const VideoTest = () => {
  const player = useRef<VideoPlayer | null>(null);
  const sb = useRef<SourceBuffer | null>(null);
  const [state, setStateRaw] = useState('initializing player');
  // Mirror state changes into the device log (loggingctl log -v com.screenatic.probe | grep VideoTest).
  const setState = (s: string) => {
    console.log(`VideoTest: ${s}`);
    setStateRaw(s);
  };
  const [appended, setAppended] = useState(0);
  const [buffered, setBuffered] = useState('');
  const [clock, setClock] = useState('');

  useEffect(() => {
    let disposed = false;
    const vp = new VideoPlayer();
    player.current = vp;
    const pending: Uint8Array[] = [];
    let total = 0;

    const t0 = Date.now();
    let initDone = false;
    const initWatch = setInterval(() => {
      if (!initDone) setState(`initializing player (${Math.round((Date.now() - t0) / 1000)} s)`);
    }, 1000);

    vp.initialize()
      .then(() => {
        initDone = true;
        setState(`player initialized in ${Date.now() - t0} ms`);
        if (disposed) return;
        const ms = new MediaSource();
        let opened = false;
        const onOpen = () => {
          if (opened) return;
          opened = true;
          try {
            const buf = ms.addSourceBuffer(MIME);
            buf.mode = AppendMode.sequence;
            buf.addEventListener('updateend', pump);
            sb.current = buf;
            setState(`source open · ${MIME} · waiting for stream on :7100`);
          } catch (e) {
            setState(`addSourceBuffer failed: ${String(e)}`);
          }
        };
        // Listen before attaching: Vega's player fires sourceopen synchronously inside the
        // srcObject setter. Attach directly rather than via src: the player only accepts
        // "blob:///<id>?..." URLs, which React Native's URL.createObjectURL does not produce.
        ms.addEventListener('sourceopen', onOpen);
        vp.srcObject = ms;
        if (ms.readyState === 'open') onOpen();
        else setState(`attached, MediaSource readyState=${ms.readyState}`);
      })
      .catch((e: unknown) => setState(`player init failed: ${String(e)}`))
      .finally(() => clearInterval(initWatch));

    function pump() {
      const buf = sb.current;
      if (!buf || buf.updating || pending.length === 0) return;
      const chunk = pending.shift()!;
      try {
        buf.appendBuffer(chunk);
      } catch (e) {
        setState(`appendBuffer failed: ${String(e)}`);
      }
    }

    const reader = setInterval(() => {
      try {
        const ab = ProbeNative.readVideo(512 * 1024);
        // Until the SourceBuffer exists nothing drains `pending`; cap it (~8 MB) so a stream that
        // arrives before the player is ready can't exhaust memory (the app was killed at ~27 MB).
        while (!sb.current && pending.length > 16) pending.shift();
        if (ab.byteLength > 0) {
          pending.push(new Uint8Array(ab));
          total += ab.byteLength;
          setAppended(total);
          pump();
          if (vp.paused) vp.play();
        }
        const b = vp.buffered;
        if (b && b.length > 0) {
          const end = b.end(b.length - 1);
          const behind = end - vp.currentTime;
          setBuffered(`t=${vp.currentTime.toFixed(2)} s, buffered to ${end.toFixed(2)} s (${behind.toFixed(2)} s ahead)`);
          // Live: never let playback drift behind the newest frame.
          if (behind > 0.5) vp.currentTime = end - 0.1;
        }
      } catch (e) {
        setState(`read failed: ${String(e)}`);
      }
    }, 30);

    const ticker = setInterval(() => {
      const d = new Date();
      setClock(`${d.toLocaleTimeString('en-GB')}.${String(d.getMilliseconds()).padStart(3, '0')}`);
    }, 50);

    return () => {
      disposed = true;
      clearInterval(reader);
      clearInterval(ticker);
      clearInterval(initWatch);
      vp.pause();
      vp.deinitialize?.();
    };
  }, []);

  return (
    <View style={styles.root}>
      <Surface player={player} />
      <Text style={styles.clock}>TV clock {clock}</Text>
      <Text style={styles.info}>{state}</Text>
      <Text style={styles.info}>
        received {Math.round(appended / 1024)} KiB · {buffered}
      </Text>
    </View>
  );
};

const styles = StyleSheet.create({
  root: {},
  surface: {width: '100%', aspectRatio: 16 / 9, backgroundColor: '#000'},
  fill: {flex: 1},
  clock: {color: '#ffd166', fontSize: 20, fontWeight: '700', marginTop: 4},
  info: {color: '#ddd', fontSize: 10},
});
