import React, {useCallback, useEffect, useState} from 'react';
import {StyleSheet, View} from 'react-native';
// Vega additions to React Native (the app's react-native resolves to this package on the device).
import {
  AsyncStorage,
  useTVEventHandler,
} from '@amazon-devices/react-native-kepler';
import type {HWEvent} from '@amazon-devices/react-native-kepler';
import {ProbeNative} from '@amazon-devices/probenative';
import {onAirPlayEvent} from './airplay';
import {DebugPanel} from './debug/DebugPanel';
import {Home} from './Home';
import {MirrorSurface, ZoomHint} from './Mirror';
import {Music} from './Music';
import {updateRequired} from './rules';
import {UpdateScreen} from './UpdateScreen';
import {UrlPlayer} from './UrlPlayer';

/** The name iPhones list under Screen Mirroring; native code adds this TV's suffix ("Screenatic TV 3F2A"). */
const DEVICE_NAME = 'Screenatic TV';
const FILL_KEY = 'mirror_fill';

/**
 * Screenatic for Vega OS: an AirPlay receiver. Layers, bottom to top: the mirror's picture
 * (always there, see MirrorSurface), AirPlay video (URL mode, full screen while it plays), the
 * home screen while nothing is cast, the update screen, and in debug builds the status panel.
 */
export const App = () => {
  const [ip, setIp] = useState('');
  const [name, setName] = useState(DEVICE_NAME);
  const [mirroring, setMirroring] = useState(false);
  const [videoActive, setVideoActive] = useState(false);
  const [musicActive, setMusicActive] = useState(false);
  const [size, setSize] = useState({width: 0, height: 0});
  const [fill, setFill] = useState(false);
  const [hintTick, setHintTick] = useState(0);
  const [mustUpdate, setMustUpdate] = useState(false);
  const [debugOpen, setDebugOpen] = useState(false);

  useEffect(() => {
    try {
      console.log(ProbeNative.startReceiver(DEVICE_NAME, __DEV__));
    } catch (e) {
      console.log(`startReceiver failed: ${String(e)}`);
    }
    const readIp = () => {
      try {
        const st = JSON.parse(ProbeNative.status());
        setIp(st?.ip ?? '');
        if (st?.name) setName(st.name);
      } catch {
        // ignore
      }
    };
    readIp();
    const ipTimer = setInterval(readIp, 10000); // Wi-Fi may come up after the app

    AsyncStorage.getItem(FILL_KEY)
      .then((v: string | null) => setFill(v === '1'))
      .catch(() => {});
    updateRequired().then((old) => {
      if (!old) return;
      setMustUpdate(true);
      ProbeNative.stop(); // stop advertising: iPhones no longer list this TV
    });

    const unsubscribe = onAirPlayEvent((ev) => {
      if (ev.type === 'mirror') {
        setMirroring(true);
        setHintTick((n) => n + 1);
      } else if (ev.type === 'disconnected') {
        setMirroring(false);
      } else if (ev.type === 'size') {
        setSize({width: ev.width, height: ev.height});
      }
    });
    return () => {
      clearInterval(ipTimer);
      unsubscribe();
      try {
        ProbeNative.stop();
      } catch {
        // ignore
      }
    };
  }, []);

  const showMirror = mirroring && !videoActive;
  useTVEventHandler(
    useCallback(
      (ev: HWEvent) => {
        if (ev.eventKeyAction !== 1) return; // key up
        if (ev.eventType === 'select' && showMirror) {
          // OK toggles fit / fill while mirroring.
          setFill((f) => {
            AsyncStorage.setItem(FILL_KEY, f ? '0' : '1').catch(() => {});
            return !f;
          });
          setHintTick((n) => n + 1);
        } else if (ev.eventType === 'menu' && __DEV__) {
          setDebugOpen((o) => !o);
        }
      },
      [showMirror],
    ),
  );

  return (
    <View style={styles.root}>
      <MirrorSurface width={size.width} height={size.height} fill={fill} />
      {showMirror ? <ZoomHint fill={fill} tick={hintTick} /> : null}
      <UrlPlayer onActive={setVideoActive} />
      {!mirroring && !videoActive && !musicActive ? (
        <Home deviceName={name} ip={ip} />
      ) : null}
      <Music onActive={setMusicActive} />
      {mustUpdate ? <UpdateScreen /> : null}
      {debugOpen ? <DebugPanel /> : null}
    </View>
  );
};

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#000'},
});
