import React, {useEffect, useRef, useState} from 'react';
import {
  findNodeHandle,
  StyleSheet,
  Text,
  useWindowDimensions,
  View,
} from 'react-native';
import {KeplerVideoSurfaceView} from '@amazon-devices/react-native-w3cmedia';
import {ProbeNative} from '@amazon-devices/probenative';
import {t} from './i18n';

// w3cmedia expects browser globals; the docs' headless example sets these before creating the player.
// eslint-disable-next-line no-undef
const g = globalThis as any;
if (typeof g.navigator === 'undefined') g.navigator = {};
if (typeof g.window === 'undefined') g.window = g;
if (typeof g.self === 'undefined') g.self = g;

/**
 * The mirror's picture. The native player (MssePlayer, MSSE mode) draws into this surface view's
 * media surface, found by its React view tag. It is always mounted, under the home screen while
 * idle, so the player has its surface before the first frame arrives.
 *
 * fill: false shows the whole picture (letter/pillarbox), true covers the screen and crops, as
 * on Android (OK on the remote toggles it).
 */
export const MirrorSurface = React.memo(
  ({width, height, fill}: {width: number; height: number; fill: boolean}) => {
    const view = useRef<any>(null);
    const attached = useRef(false);
    const screen = useWindowDimensions();
    let w = screen.width;
    let h = screen.height;
    if (width > 0 && height > 0) {
      const video = width / height;
      const matchWidth = video > screen.width / screen.height !== fill;
      if (matchWidth) h = screen.width / video;
      else w = screen.height * video;
    }
    return (
      <View style={styles.root} pointerEvents="none">
        <View
          style={{width: w, height: h}}
          onLayout={() => {
            // Native keeps its media surface view the size of ours; tell it after each resize.
            if (!attached.current) return;
            const handle = String(findNodeHandle(view.current) ?? '');
            setTimeout(() => {
              const r = ProbeNative.msseAttach(handle);
              ProbeNative.report('surface', JSON.stringify({handle, msse: r}));
            }, 100);
          }}>
          <KeplerVideoSurfaceView
            ref={view}
            style={styles.fill}
            onSurfaceViewCreated={() => {
              if (attached.current) return;
              attached.current = true;
              const handle = String(findNodeHandle(view.current) ?? '');
              // Attaching right away found no media surface yet on the first launch.
              setTimeout(() => {
                const r = ProbeNative.msseAttach(handle);
                ProbeNative.report(
                  'surface',
                  JSON.stringify({handle, msse: r}),
                );
              }, 500);
            }}
          />
        </View>
      </View>
    );
  },
);

/** "Fit / Fill · press OK" for 3 s after each toggle and at the start of a session. */
export const ZoomHint = ({fill, tick}: {fill: boolean; tick: number}) => {
  const [visible, setVisible] = useState(false);
  useEffect(() => {
    if (tick === 0) return;
    setVisible(true);
    const timer = setTimeout(() => setVisible(false), 3000);
    return () => clearTimeout(timer);
  }, [tick]);
  if (!visible) return null;
  return (
    <View style={styles.hintBox} pointerEvents="none">
      <Text style={styles.hint}>{t(fill ? 'zoom_fill' : 'zoom_fit')}</Text>
    </View>
  );
};

const styles = StyleSheet.create({
  root: {
    ...StyleSheet.absoluteFillObject,
    backgroundColor: '#000',
    alignItems: 'center',
    justifyContent: 'center',
    overflow: 'hidden',
  },
  fill: {flex: 1},
  hintBox: {
    position: 'absolute',
    top: 24,
    left: 0,
    right: 0,
    alignItems: 'center',
  },
  hint: {
    color: '#fff',
    fontSize: 16,
    backgroundColor: 'rgba(0,0,0,0.7)',
    borderRadius: 8,
    paddingHorizontal: 20,
    paddingVertical: 10,
    overflow: 'hidden',
  },
});
