import React, {useEffect, useRef, useState} from 'react';
import {Animated, Easing, Image, StyleSheet, Text, View} from 'react-native';
import {
  KeplerVideoSurfaceView,
  VideoPlayer,
} from '@amazon-devices/react-native-w3cmedia';
import {ProbeNative} from '@amazon-devices/probenative';
import {AppState} from '@amazon-devices/react-native-kepler';
import {MediaKind, onAirPlayEvent, PlayEvent, Source} from './airplay';
import {t} from './i18n';
import {NowPlaying} from './NowPlaying';

/**
 * Media by URL: AirPlay video (URL mode: YouTube, Safari fullscreen) and DLNA (Android phones,
 * Windows: video, music, photos). The TV fetches the media itself; Vega's player streams it
 * natively (no MSE, so none of the mirror's buffer limits). Its own player and surface, created
 * per item and torn down when the sender stops. Photos are an Image, music shows what's playing.
 */
// Kept across videos so a failed attempt can still be read from :7000/status afterwards.
const history: string[] = [];

export const UrlPlayer = ({
  onActive,
}: {
  onActive: (active: boolean) => void;
}) => {
  const [active, setActiveState] = useState(false);
  const [failed, setFailed] = useState(false);
  const onActiveRef = useRef(onActive);
  onActiveRef.current = onActive;
  const setActive = (a: boolean) => {
    setActiveState(a);
    onActiveRef.current(a);
  };
  // Until the URL player shows its first frame, its layer stays see-through and the mirror player
  // keeps its last frame on screen (instead of ~2 s of black while the video loads).
  const [shown, setShown] = useState(false);
  const player = useRef<VideoPlayer | null>(null);
  const surface = useRef<string | null>(null);
  /** Who is on screen: commands and volume from another sender are ignored. */
  const session = useRef<{source: Source; kind: MediaKind} | null>(null);
  const failedRef = useRef(false);
  const [media, setMedia] = useState<PlayEvent | null>(null);

  useEffect(() => {
    let reporter: ReturnType<typeof setInterval> | null = null;
    let finished = false;
    let startedAt = 0;

    /** closed: the item leaves the screen for a reason its sender didn't ask for (another sender,
     *  mirroring resumed); DLNA then reports STOPPED to its controller. */
    const stop = (why: string, closed = true) => {
      if (reporter) clearInterval(reporter);
      reporter = null;
      const was = session.current;
      session.current = null;
      const vp = player.current;
      player.current = null;
      if (vp) {
        // Give the decoder back to the native mirror player once this player is torn down
        // (not when stop() is only switching to the next video). Resuming after a fixed 150 ms
        // brought the picture back but left the mirror silent for good: wait for the deinit
        // (the audio output is free only then), at most 1.5 s.
        const t0 = Date.now();
        let resumed = false;
        const resume = (how: string) => {
          if (resumed) return;
          resumed = true;
          if (player.current) return;
          const r = ProbeNative.msseAttach('cmd:suspend=0');
          history.push(
            `${new Date().toLocaleTimeString('en-GB')} mirror resumed ${
              Date.now() - t0
            } ms after stop (${how}): ${r}`,
          );
          while (history.length > 15) history.shift();
          ProbeNative.report(
            'url',
            JSON.stringify({state: 'stopped', history}),
          );
        };
        const timer = setTimeout(() => resume('timeout'), 1500);
        let deinit: Promise<void> | undefined;
        try {
          vp.pause();
          if (surface.current) vp.clearSurfaceHandle(surface.current);
          deinit = vp.deinitialize?.();
        } catch {
          // ignore
        }
        Promise.resolve(deinit)
          .catch(() => {})
          .then(() => {
            clearTimeout(timer);
            resume(deinit ? 'deinitialized' : 'no deinit');
          });
      }
      ProbeNative.setPlayback(
        [
          0,
          0,
          0,
          0,
          0,
          0,
          0,
          was?.source ?? 'airplay',
          was && closed ? 1 : 0,
        ].join(','),
      );
      setActive(false);
      setMedia(null);
      if (vp || was) {
        history.push(`${new Date().toLocaleTimeString('en-GB')} stop: ${why}`);
        while (history.length > 15) history.shift();
        ProbeNative.report(
          'url',
          JSON.stringify({state: `stopped: ${why}`, history}),
        );
      }
    };

    const start = (ev: PlayEvent) => {
      const source: Source = ev.source ?? 'airplay';
      const kind: MediaKind = ev.mediaType ?? 'video';
      // Same sender, next item: not "closed" for it (DLNA moves on to the new URI itself).
      stop('switching media', session.current?.source !== source);
      startedAt = Date.now();
      finished = false;
      failedRef.current = false;
      session.current = {source, kind};
      setMedia(ev);
      const csv = (
        dur: number,
        pos: number,
        playing: boolean,
        ready: boolean,
        buffering: boolean,
      ) =>
        [
          dur,
          pos,
          playing ? 1 : 0,
          ready ? 1 : 0,
          buffering ? 1 : 0,
          finished ? 1 : 0,
          failedRef.current ? 1 : 0,
          source,
          0,
        ].join(',');
      if (kind === 'image') {
        // A photo: no player, and the mirror keeps its decoder.
        setShown(true);
        setFailed(false);
        setActive(true);
        history.push(
          `${new Date().toLocaleTimeString('en-GB')} image ${ev.url.slice(
            0,
            140,
          )}`,
        );
        while (history.length > 15) history.shift();
        ProbeNative.report('url', JSON.stringify({state: 'image', history}));
        reporter = setInterval(
          () => ProbeNative.setPlayback(csv(0, 0, true, true, false)),
          1000,
        );
        return;
      }
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
      setFailed(false);
      setActive(true);
      setTimeout(() => handOver('2.5 s timeout'), 2500);
      console.log(`UrlPlayer: play ${ev.url}`);
      const vp = new VideoPlayer();
      player.current = vp;
      const report = (s: string) => {
        history.push(
          `${new Date().toLocaleTimeString('en-GB')} ${s.slice(0, 160)}`,
        );
        while (history.length > 15) history.shift();
        let pos = -1;
        let rs = -1;
        try {
          pos = vp.currentTime;
          rs = vp.readyState;
        } catch {
          // ignore
        }
        ProbeNative.report(
          'url',
          JSON.stringify({
            url: ev.url.slice(0, 300),
            state: s,
            t: pos,
            rs,
            history,
          }),
        );
      };
      report(`received ${ev.url.slice(0, 160)} (start ${ev.startMs} ms)`);
      vp.addEventListener('error', () => {
        failedRef.current = true;
        const err = (vp as any).error;
        report(`player error: code ${err?.code ?? '?'} ${err?.message ?? ''}`);
        if (player.current === vp) {
          setFailed(true);
          handOver('player error'); // show the message instead of a spinner over a frozen mirror
        }
      });
      vp.addEventListener('loadedmetadata', () =>
        report(`metadata: duration ${vp.duration}`),
      );
      vp.addEventListener('ended', () => {
        finished = true;
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
          vp.volume = ev.gain; // the TV's saved volume (AirPlay or DLNA)
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

      // Stall watchdog: Vega's player sometimes stops for good in the middle of an HLS stream
      // (readyState 2, position frozen; seen after 1-5 min of Safari video). Reload the stream at
      // the same position once it has stood still for 8 s while meant to play.
      let lastPos = -1;
      let stillSince = 0;
      let reloads = 0;
      const watchdog = (p: VideoPlayer) => {
        const pos = p.currentTime;
        if (p.paused || finished || p.readyState >= 3 || pos !== lastPos) {
          if (pos > lastPos + 10) reloads = 0; // playing again: allow later stalls their retries
          lastPos = pos;
          stillSince = Date.now();
          return;
        }
        if (Date.now() - stillSince < 8000 || reloads >= 5) return;
        stillSince = Date.now();
        ++reloads;
        report(`stalled at ${pos.toFixed(1)} s: reloading (${reloads})`);
        try {
          const resume = () => {
            p.currentTime = pos;
            p.removeEventListener('loadedmetadata', resume);
            p.play();
          };
          p.addEventListener('loadedmetadata', resume);
          p.src = ev.url;
          p.play();
        } catch (e) {
          report(`reload failed: ${String(e)}`);
        }
      };

      // The sender polls /playback-info about once a second; keep its view current.
      let ticks = 0;
      reporter = setInterval(() => {
        const p = player.current;
        if (!p) return;
        watchdog(p);
        if (++ticks % 6 === 0)
          report(
            `tick: t=${p.currentTime.toFixed(1)} readyState=${
              p.readyState
            } paused=${p.paused}`,
          );
        const dur = Number.isFinite(p.duration) ? p.duration : 0;
        const ready = p.readyState >= 2;
        const buffering = !p.paused && p.readyState < 3;
        ProbeNative.setPlayback(
          csv(dur, p.currentTime, !p.paused, ready, buffering),
        );
      }, 500);
    };

    // A player started in the background gets the app killed: wait for the foreground (App.tsx
    // asks for it as soon as a sender casts).
    let pending: PlayEvent | null = null;
    const appStateSub = AppState.addEventListener('change', (state: string) => {
      // Playing on in the background gets the app killed: stop (the sender sees it end).
      if (state === 'background' && session.current) {
        stop('app sent to the background');
        return;
      }
      if (state === 'active' && pending) {
        const ev = pending;
        pending = null;
        start(ev);
      }
    });

    const unsubscribe = onAirPlayEvent((ev) => {
      if (ev.type === 'play') {
        if (AppState.currentState !== 'active') {
          pending = ev;
          return;
        }
        start(ev);
      } else if (ev.type === 'cmd') {
        if ((ev.source ?? 'airplay') !== session.current?.source) return;
        if (ev.cmd === 2) return stop('stopped by sender', false);
        const vp = player.current;
        if (!vp) return;
        if (ev.cmd === 0) vp.currentTime = ev.value;
        else if (ev.cmd === 1) ev.value > 0 ? vp.play() : vp.pause();
      } else if (ev.type === 'volume') {
        if ((ev.source ?? 'airplay') !== session.current?.source) return;
        if (player.current) player.current.volume = ev.gain;
      } else if (ev.type === 'mirror' && session.current) {
        // Mirroring resumed: the iPhone left fullscreen, sometimes with only a pause (rate=0)
        // and no stop. Keeping the paused video froze the mirror (it never got its decoder back).
        // (Just after a URL arrives the mirror is still being torn down, not resumed.)
        if (Date.now() - startedAt < 1500) return;
        stop('mirror resumed');
      } else if (
        ev.type === 'disconnected' &&
        session.current?.source === 'airplay'
      ) {
        // Handing a video over (Safari fullscreen), the iPhone closes its mirror connection
        // right after sending the URL: that disconnect is not the end of this video.
        if (Date.now() - startedAt < 3000) return;
        stop('sender disconnected');
      }
    });

    return () => {
      appStateSub.remove();
      unsubscribe();
      stop('unmounted');
    };
  }, []);

  if (!active) return null;
  if (media?.mediaType === 'image') {
    return (
      <View style={styles.overlay} pointerEvents="none">
        <Image
          source={{uri: media.url}}
          style={styles.fill}
          resizeMode="contain"
          onError={() => {
            failedRef.current = true;
            setFailed(true);
          }}
        />
        {failed ? (
          <View style={styles.errorBox}>
            <Text style={styles.error}>{t('media_failed')}</Text>
          </View>
        ) : null}
      </View>
    );
  }
  return (
    <>
      {!shown ? (
        // Loading: a spinner over the mirror's last frame, so the frozen picture reads as "loading".
        <View style={styles.loading} pointerEvents="none">
          <View style={styles.spinnerBox}>
            <Spinner />
          </View>
        </View>
      ) : null}
      <View
        style={shown ? styles.overlay : styles.overlayHidden}
        pointerEvents="none">
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
        {media?.mediaType === 'audio' && !failed ? (
          <NowPlaying
            title={media.title}
            artist={media.artist}
            album={media.album}
            artworkUri={media.artworkUrl || undefined}
            client={media.client}
          />
        ) : null}
        {failed ? (
          <View style={styles.errorBox}>
            <Text style={styles.error}>{t('media_failed')}</Text>
          </View>
        ) : null}
      </View>
    </>
  );
};

/** A rotating ring (Vega's ActivityIndicator draws three blinking dots instead). */
const Spinner = () => {
  const turn = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    const loop = Animated.loop(
      Animated.timing(turn, {
        toValue: 1,
        duration: 900,
        easing: Easing.linear,
        useNativeDriver: true,
      }),
    );
    loop.start();
    return () => loop.stop();
  }, [turn]);
  const rotate = turn.interpolate({
    inputRange: [0, 1],
    outputRange: ['0deg', '360deg'],
  });
  return <Animated.View style={[styles.ring, {transform: [{rotate}]}]} />;
};

const styles = StyleSheet.create({
  // Full screen over the mirror (the two never play at once).
  overlay: {...StyleSheet.absoluteFillObject, backgroundColor: '#000'},
  // Loading: same size (the player needs its surface) but off screen: a native surface view
  // clears its area to black when created, which showed through opacity 0.
  overlayHidden: {
    position: 'absolute',
    top: 0,
    left: -3000,
    width: 960,
    height: 540,
  },
  fill: {flex: 1},
  loading: {
    ...StyleSheet.absoluteFillObject,
    alignItems: 'center',
    justifyContent: 'center',
    zIndex: 10,
  },
  errorBox: {
    ...StyleSheet.absoluteFillObject,
    alignItems: 'center',
    justifyContent: 'center',
  },
  error: {
    color: '#fff',
    fontSize: 16,
    backgroundColor: 'rgba(0,0,0,0.8)',
    borderRadius: 12,
    padding: 24,
    overflow: 'hidden',
  },
  ring: {
    width: 48,
    height: 48,
    borderRadius: 24,
    borderWidth: 4,
    borderColor: 'rgba(255,255,255,0.25)',
    borderTopColor: '#ffffff',
  },
  spinnerBox: {
    padding: 18,
    borderRadius: 16,
    backgroundColor: 'rgba(0,0,0,0.55)',
  },
});
