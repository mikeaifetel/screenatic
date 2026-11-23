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
  const find = (fourcc: string, from = 0) => {
    const c = [...fourcc].map(ch => ch.charCodeAt(0));
    for (let i = from; i + 4 <= init.length; i++) {
      if (init[i] === c[0] && init[i + 1] === c[1] && init[i + 2] === c[2] && init[i + 3] === c[3]) return i;
    }
    return -1;
  };
  // avcC payload: version, profile, compatibility, level -> "avc1.PPCCLL"
  let video = 'avc1.4d401f';
  const avcC = find('avcC');
  if (avcC >= 0 && avcC + 8 <= init.length) {
    const hex = (v: number) => v.toString(16).padStart(2, '0');
    video = `avc1.${hex(init[avcC + 5])}${hex(init[avcC + 6])}${hex(init[avcC + 7])}`;
  }
  const audio = find('ipcm') >= 0 ? 'ipcm' : find('fLaC') >= 0 ? 'flac' : find('mp4a') >= 0 ? 'mp4a.40.2' : '';
  return `video/mp4; codecs="${video}${audio ? ', ' + audio : ''}"`;
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
  const [events, setEvents] = useState('');
  const [diag, setDiag] = useState('');

  useEffect(() => {
    let disposed = false;
    const vp = new VideoPlayer();
    player.current = vp;
    const pending: Uint8Array[] = [];
    let pendingBytes = 0;
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
        attachSource();
      })
      .catch((e: unknown) => setState(`player init failed: ${String(e)}`))
      .finally(() => clearInterval(initWatch));

    function attachSource() {
      const ms = new MediaSource();
      let opened = false;
      msRef.current = null;
      sb.current = null;
      const onOpen = () => {
        if (opened) return;
        opened = true;
        msRef.current = ms;
        setState('source open · waiting for stream');
      };
      // Listen before attaching: Vega's player fires sourceopen synchronously inside the
      // srcObject setter. Attach directly rather than via src: the player only accepts
      // "blob:///<id>?..." URLs, which React Native's URL.createObjectURL does not produce.
      ms.addEventListener('sourceopen', onOpen);
      vp.srcObject = ms;
      vp.playbackRate = 1;
      if (ms.readyState === 'open') onOpen();
      else setState(`attached, MediaSource readyState=${ms.readyState}`);
    }

    function pump() {
      const buf = sb.current;
      if (!buf || buf.updating || pending.length === 0) return;
      const chunk = pending.shift()!;
      pendingBytes -= chunk.byteLength;
      try {
        buf.appendBuffer(chunk);
      } catch (e) {
        setState(`appendBuffer failed: ${String(e)}`);
      }
    }

    // How does the player behave at the buffer end? Count stalls and note how much was buffered
    // when it resumed: that is the cushion live playback has to keep.
    let waits = 0;
    let lastResumeAhead = -1;
    const ahead = () => {
      const b = vp.buffered;
      return b && b.length > 0 ? b.end(b.length - 1) - vp.currentTime : 0;
    };
    vp.addEventListener('waiting', () => {
      waits++;
      setEvents(`waiting x${waits}, last resume at ${lastResumeAhead.toFixed(2)} s ahead`);
    });
    vp.addEventListener('playing', () => {
      lastResumeAhead = ahead();
      setEvents(`waiting x${waits}, last resume at ${lastResumeAhead.toFixed(2)} s ahead`);
    });

    // Diagnostics for stutter: fragments arriving per second, decoder drops, rate switches.
    let fragsThisSec = 0;
    let fragsPerSec = 0;
    let rateSwitches = 0;
    let trims = 0;
    let lastRate = 1;
    const diag = setInterval(() => {
      fragsPerSec = fragsThisSec;
      fragsThisSec = 0;
      let q = '';
      try {
        const pq = (vp as any).getVideoPlaybackQuality?.();
        if (pq) q = `, dropped ${pq.droppedVideoFrames}/${pq.totalVideoFrames}`;
      } catch {}
      setDiag(`in ${fragsPerSec} frames/s${q}, rate switches ${rateSwitches}, trimmed ${trims}x`);
      // Keep the SourceBuffer from filling up (it froze at ~100 MB): drop what lies before the
      // latest keyframe that is at least 1 s behind the playhead. Removing frames also removes
      // everything depending on them up to the next keyframe, so cutting anywhere else would
      // take the picture with it. The iPhone sends a keyframe about every 10 s.
      try {
        const buf = sb.current;
        const br = vp.buffered;
        if (buf && !buf.updating && br && br.length > 0) {
          const keyTimes: number[] = JSON.parse(ProbeNative.status())?.airplay?.video?.keyTimes ?? [];
          const start = br.start(0);
          const cut = keyTimes.filter(k => k <= vp.currentTime - 1 && k > start + 1).pop();
          if (cut !== undefined && cut - start > 15) {
            buf.remove(start, cut);
            trims++;
          }
        }
      } catch {}
      try {
        const b = vp.buffered;
        const pq = (vp as any).getVideoPlaybackQuality?.();
        ProbeNative.report(
          'video',
          JSON.stringify({
            t: vp.currentTime,
            bufferedStart: b && b.length > 0 ? b.start(0) : null,
            bufferedEnd: b && b.length > 0 ? b.end(b.length - 1) : null,
            ranges: b ? b.length : 0,
            rate: vp.playbackRate,
            paused: vp.paused,
            waits,
            lastResumeAhead,
            framesPerSec: fragsPerSec,
            dropped: pq?.droppedVideoFrames ?? null,
            decoded: pq?.totalVideoFrames ?? null,
            received: total,
            pendingBytes,
            sourceBuffer: !!sb.current,
            updating: sb.current?.updating ?? null,
          }),
        );
      } catch {}
    }, 1000);

    const reader = setInterval(() => {
      try {
        const ab = ProbeNative.readVideo(512 * 1024);
        // Until the SourceBuffer exists, keep everything: an AirPlay mirror sends its only keyframe
        // right after connecting, and dropping any chunk before it is appended leaves the stream
        // undecodable (the sender often connects while the player is still initializing). Only a
        // runaway backlog (player never came up) is discarded, to keep memory bounded.
        if (!sb.current && pendingBytes > 20 * 1024 * 1024) {
          pending.length = 0;
          pendingBytes = 0;
          setState('player not ready, dropped 20 MB backlog');
        }
        if (ab.byteLength > 0) {
          const chunk = new Uint8Array(ab);
          pending.push(chunk);
          pendingBytes += ab.byteLength;
          // count moof boxes = frames (one per fragment)
          for (let i = 4; i + 4 <= chunk.length; i++) {
            if (chunk[i] === 0x6d && chunk[i + 1] === 0x6f && chunk[i + 2] === 0x6f && chunk[i + 3] === 0x66) fragsThisSec++;
          }
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
          // Live: catch up by playing faster, not by seeking. A seek makes the decoder restart
          // from the previous keyframe, and AirPlay mirroring sends keyframes only every few
          // seconds (or only on start), so seeking froze the picture again and again.
          if (behind > 3) {
            vp.currentTime = end - 0.1;  // hopelessly behind (e.g. after a stall): accept the hitch
          } else if (behind > 0.9 && vp.playbackRate !== 1.2) {
            // Vega's player resumes from a stall only with ~0.5 s buffered; at a 0.45 s cushion it
            // stalled ~20 times in 40 s, so keep ~0.6 s and burn off only what is above that.
            // 1.2x, not 1.5x: catching up on a 1080p stream at 1.5x (45 fps) overloaded the decoder.
            vp.playbackRate = 1.2;
          } else if (behind < 0.6 && vp.playbackRate !== 1) {
            vp.playbackRate = 1;
          }
          if (vp.playbackRate !== lastRate) {
            lastRate = vp.playbackRate;
            rateSwitches++;
          }
          setBuffered(`t=${vp.currentTime.toFixed(2)} s, buffered to ${end.toFixed(2)} s (${behind.toFixed(2)} s ahead) rate ${vp.playbackRate}`);
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
      clearInterval(diag);
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
      <Text style={styles.info}>player: {events}</Text>
      <Text style={styles.info}>video: {diag}</Text>
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
