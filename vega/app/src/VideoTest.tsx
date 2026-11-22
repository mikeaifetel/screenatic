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

// What a mirroring session will feed: H.264 Baseline/Main in fragmented MP4.
const MIME = 'video/mp4; codecs="avc1.4d401f"';

/**
 * MSE latency test: bytes arriving on TCP :7100 (an fMP4 stream from ffmpeg on the Mac, with the
 * Mac's wall clock burned into the picture) are appended to a SourceBuffer. Comparing the burned-in
 * clock with the TV clock shown below gives glass-to-glass latency.
 */
export const VideoTest = () => {
  const player = useRef<VideoPlayer | null>(null);
  const sb = useRef<SourceBuffer | null>(null);
  const [state, setState] = useState('initializing player');
  const [appended, setAppended] = useState(0);
  const [buffered, setBuffered] = useState('');
  const [clock, setClock] = useState('');

  useEffect(() => {
    let disposed = false;
    const vp = new VideoPlayer();
    player.current = vp;
    const pending: Uint8Array[] = [];
    let total = 0;

    vp.initialize()
      .then(() => {
        if (disposed) return;
        const ms = new MediaSource();
        vp.src = URL.createObjectURL(ms);
        ms.addEventListener('sourceopen', () => {
          try {
            const buf = ms.addSourceBuffer(MIME);
            buf.mode = AppendMode.sequence;
            buf.addEventListener('updateend', pump);
            sb.current = buf;
            setState(`source open · ${MIME} · waiting for stream on :7100`);
          } catch (e) {
            setState(`addSourceBuffer failed: ${String(e)}`);
          }
        });
      })
      .catch((e: unknown) => setState(`player init failed: ${String(e)}`));

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
    }, 33);

    return () => {
      disposed = true;
      clearInterval(reader);
      clearInterval(ticker);
      vp.pause();
      vp.deinitialize?.();
    };
  }, []);

  return (
    <View style={styles.root}>
      <View style={styles.surface}>
        <KeplerVideoSurfaceView
          style={styles.fill}
          onSurfaceViewCreated={(h: string) => player.current?.setSurfaceHandle(h)}
          onSurfaceViewDestroyed={(h: string) => player.current?.clearSurfaceHandle(h)}
        />
      </View>
      <Text style={styles.clock}>TV clock {clock}</Text>
      <Text style={styles.info}>{state}</Text>
      <Text style={styles.info}>
        received {Math.round(appended / 1024)} KiB · {buffered}
      </Text>
    </View>
  );
};

const styles = StyleSheet.create({
  root: {flex: 1},
  surface: {width: 768, height: 432, backgroundColor: '#000'},
  fill: {flex: 1},
  clock: {color: '#ffd166', fontSize: 30, fontWeight: '700', marginTop: 8},
  info: {color: '#ddd', fontSize: 15},
});
