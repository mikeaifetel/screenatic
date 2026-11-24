import React, {useEffect, useState} from 'react';
import {StyleSheet, Text} from 'react-native';
import {AppendMode, AudioPlayer, MediaSource, SourceBuffer} from '@amazon-devices/react-native-w3cmedia';
import {ProbeNative} from '@amazon-devices/probenative';

const MIME = 'audio/mp4; codecs="flac"';
// The mirror's sound now plays in the native MSSE player, as a second track next to the video
// (MssePlayer::pushAudio). This MSE player then only drains the queue and shows the counters.
const USE_MSSE_AUDIO = true;

/**
 * Plays the mirror's audio: native code decodes AirPlay's AAC-ELD, re-encodes it as FLAC (the
 * audio format Vega's MSE plays) and hands over an audio-only fMP4 stream via readAudio().
 *
 * It has its own player, separate from the video's, because a still iPhone screen sends no video
 * frames for seconds: with audio and video in one SourceBuffer the player stalls both.
 * Every FLAC frame decodes on its own, so falling behind is fixed with a plain seek (video can't
 * do that: it would wait for the next keyframe).
 */
export const MirrorAudio = () => {
  const [state, setState] = useState('initializing audio player');
  const [info, setInfo] = useState('');

  useEffect(() => {
    if (USE_MSSE_AUDIO) {
      setState('native MSSE audio track');
      const t = setInterval(() => {
        try {
          ProbeNative.readAudio(1024 * 1024);
          const m = JSON.parse(ProbeNative.status())?.airplay?.video?.msse ?? {};
          setInfo(`track ${m.audioTrack ? 'yes' : 'no'} · queued ${m.audioQueued ?? 0} frames, errors ${m.audioErrors ?? 0}, re-anchored ${m.audioReanchors ?? 0}x`);
        } catch {}
      }, 500);
      return () => clearInterval(t);
    }
    let disposed = false;
    const ap = new AudioPlayer();
    let sb: SourceBuffer | null = null;
    let ms: MediaSource | null = null;
    const pending: Uint8Array[] = [];
    let pendingBytes = 0;
    let total = 0;
    let seeks = 0;
    let ticks = 0;

    function pump() {
      if (!sb || sb.updating || pending.length === 0) return;
      const chunk = pending.shift()!;
      pendingBytes -= chunk.byteLength;
      try {
        sb.appendBuffer(chunk);
      } catch (e) {
        setState(`appendBuffer failed: ${String(e)}`);
      }
    }

    function attachSource() {
      const source = new MediaSource();
      let opened = false;
      ms = null;
      sb = null;
      const onOpen = () => {
        if (opened) return;
        opened = true;
        ms = source;
        setState('waiting for mirror audio');
      };
      // As for video: listen first (sourceopen fires inside the srcObject setter).
      source.addEventListener('sourceopen', onOpen);
      ap.srcObject = source;
      if (source.readyState === 'open') onOpen();
    }

    ap.initialize()
      .then(() => {
        if (disposed) return;
        attachSource();
      })
      .catch((e: unknown) => setState(`audio player init failed: ${String(e)}`));

    const reader = setInterval(() => {
      try {
        const ab = ProbeNative.readAudio(256 * 1024);
        if (ab.byteLength > 0) {
          const chunk = new Uint8Array(ab);
          const fresh = chunk.length >= 8 && chunk[4] === 0x66 && chunk[5] === 0x74 && chunk[6] === 0x79 && chunk[7] === 0x70;
          if (fresh && sb) {
            // New stream: drop the old backlog, keep the SourceBuffer (see VideoTest).
            pending.length = 0;
            pendingBytes = 0;
          }
          pending.push(chunk);
          pendingBytes += ab.byteLength;
          total += ab.byteLength;
          if (!sb && ms) {
            try {
              sb = ms.addSourceBuffer(MIME);
              sb.mode = AppendMode.sequence;
              sb.addEventListener('updateend', pump);
              setState(`playing · ${MIME}`);
            } catch (e) {
              setState(`addSourceBuffer(${MIME}) failed: ${String(e)}`);
              ms = null;
            }
          }
          if (!sb && pendingBytes > 4 * 1024 * 1024) {
            pending.length = 0;
            pendingBytes = 0;
          }
          pump();
          if (sb && ap.paused) ap.play();
        }
        // Every query of buffered writes a log line (and each app keeps ~100): check ~2x a second.
        if (++ticks % 16 !== 0) return;
        const b = ap.buffered;
        if (b && b.length > 0) {
          const end = b.end(b.length - 1);
          const behind = end - ap.currentTime;
          // Keep ~0.4 s queued (about what the video needs, so lips roughly match). A backlog
          // after a network stall is skipped, not played fast: sped-up speech sounds wrong.
          if (sb && !sb.updating && b.start(0) < ap.currentTime - 15) {
            try {
              sb.remove(0, ap.currentTime - 10);  // keep the SourceBuffer from filling up
            } catch {}
          }
          if (behind > 1.0) {
            ap.currentTime = end - 0.4;
            seeks++;
          }
          setInfo(
            `${Math.round(total / 1024)} KiB · t=${ap.currentTime.toFixed(2)} s, ${behind.toFixed(2)} s queued, ${seeks} skips`,
          );
        }
      } catch (e) {
        setState(`audio read failed: ${String(e)}`);
      }
    }, 30);

    return () => {
      disposed = true;
      clearInterval(reader);
      ap.pause();
      ap.deinitialize?.();
    };
  }, []);

  return (
    <>
      <Text style={styles.info}>audio: {state}</Text>
      <Text style={styles.info}>audio: {info}</Text>
    </>
  );
};

const styles = StyleSheet.create({
  info: {color: '#ddd', fontSize: 10},
});
