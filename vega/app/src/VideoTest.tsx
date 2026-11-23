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
// The SourceBuffer type is picked from the stream's init segment (moov), so one build can test
// both audio paths stream_test.sh produces: FLAC (default) or LPCM ("ipcm", AUDIO=pcm).
function mimeFor(init: Uint8Array): string {
  const has = (fourcc: string) => {
    const c = [...fourcc].map(ch => ch.charCodeAt(0));
    for (let i = 0; i + 4 <= init.length; i++) {
      if (init[i] === c[0] && init[i + 1] === c[1] && init[i + 2] === c[2] && init[i + 3] === c[3]) return true;
    }
    return false;
  };
  const audio = has('ipcm') ? 'ipcm' : has('fLaC') ? 'flac' : has('mp4a') ? 'mp4a.40.2' : '';
  return `video/mp4; codecs="avc1.4d401f${audio ? ', ' + audio : ''}"`;
}

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
  const msRef = useRef<MediaSource | null>(null);
  const [state, setStateRaw] = useState('initializing player');
  // Mirror state changes into the device log (loggingctl log -v com.screenatic.probe | grep VideoTest).
  const setState = (s: string) => {
    console.log(`VideoTest: ${s}`);
    setStateRaw(s);
  };
  const [appended, setAppended] = useState(0);
  const [buffered, setBuffered] = useState('');
  const [clock, setClock] = useState('');
  const [codecs, setCodecs] = useState('');

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
        // Which audio formats can MSE take? AirPlay mirroring sends AAC-ELD, audio-only AirPlay ALAC.
        const probes: [string, string][] = [
          ['H.264', 'video/mp4; codecs="avc1.4d401f"'],
          ['AAC-LC', 'audio/mp4; codecs="mp4a.40.2"'],
          ['HE-AAC', 'audio/mp4; codecs="mp4a.40.5"'],
          ['AAC-ELD', 'audio/mp4; codecs="mp4a.40.39"'],
          ['ALAC', 'audio/mp4; codecs="alac"'],
          ['FLAC', 'audio/mp4; codecs="flac"'],
          ['Opus', 'audio/mp4; codecs="opus"'],
          ['LPCM', 'audio/mp4; codecs="lpcm"'],
          ['ipcm', 'audio/mp4; codecs="ipcm"'],
          ['AC-3', 'audio/mp4; codecs="ac-3"'],
          ['WebM Opus', 'audio/webm; codecs="opus"'],
        ];
        const result = probes
          .map(([n, m]) => {
            let ok = false;
            try {
              ok = MediaSource.isTypeSupported(m);
            } catch {}
            return `${n}:${ok ? 'yes' : 'no'}`;
          })
          .join(' ');
        console.log(`VideoTest codecs: ${result}`);
        setCodecs(result);
        if (disposed) return;
        const ms = new MediaSource();
        let opened = false;
        const onOpen = () => {
          if (opened) return;
          opened = true;
          msRef.current = ms;
          setState('source open · waiting for stream on :7100');
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
        // Keep pending[0]: it starts with the init segment the SourceBuffer needs first.
        while (!sb.current && pending.length > 16) pending.splice(1, 1);
        if (ab.byteLength > 0) {
          pending.push(new Uint8Array(ab));
          if (!sb.current && msRef.current) {
            const mime = mimeFor(pending[0]);
            try {
              const buf = msRef.current.addSourceBuffer(mime);
              buf.mode = AppendMode.sequence;
              buf.addEventListener('updateend', pump);
              sb.current = buf;
              setState(`playing · ${mime}`);
            } catch (e) {
              setState(`addSourceBuffer(${mime}) failed: ${String(e)}`);
              msRef.current = null;
            }
          }
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
      <Text style={styles.info}>MSE: {codecs}</Text>
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
