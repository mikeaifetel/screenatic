import React from 'react';
import {Image, StyleSheet, Text, View} from 'react-native';
import {t} from './i18n';
import {colors} from './theme';

/** Music on the TV (AirPlay audio-only, DLNA): cover, title, artist · album, who is casting. */
export const NowPlaying = ({
  title,
  artist,
  album,
  artworkUri,
  client,
  position,
  duration,
  paused,
}: {
  title?: string;
  artist?: string;
  album?: string;
  artworkUri?: string;
  client?: string;
  /** Seconds; the bar shows only with a duration. */
  position?: number;
  duration?: number;
  paused?: boolean;
}) => {
  const fraction =
    duration && duration > 0
      ? Math.min(1, Math.max(0, (position ?? 0) / duration))
      : -1;
  return (
    <View style={styles.root}>
      <Image
        source={artworkUri ? {uri: artworkUri} : require('./assets/logo.png')}
        style={styles.cover}
      />
      <View style={styles.text}>
        <Text style={styles.title} numberOfLines={2}>
          {title || t('now_playing_unknown')}
        </Text>
        {artist || album ? (
          <Text style={styles.sub} numberOfLines={2}>
            {[artist, album].filter(Boolean).join(' · ')}
          </Text>
        ) : null}
        {fraction >= 0 ? (
          <View style={styles.progress}>
            <View style={styles.track}>
              <View style={[styles.bar, {width: `${fraction * 100}%`}]} />
            </View>
            <Text style={styles.time}>
              {paused ? '❚❚  ' : ''}
              {clock(position ?? 0)} / {clock(duration ?? 0)}
            </Text>
          </View>
        ) : null}
        {client ? (
          <Text style={styles.from}>{t('now_playing_from', client)}</Text>
        ) : null}
      </View>
    </View>
  );
};

function clock(sec: number): string {
  const s = Math.max(0, Math.floor(sec));
  const m = Math.floor(s / 60);
  return `${m}:${String(s % 60).padStart(2, '0')}`;
}

const styles = StyleSheet.create({
  root: {
    ...StyleSheet.absoluteFillObject,
    backgroundColor: colors.background,
    flexDirection: 'row',
    alignItems: 'center',
    padding: 64,
  },
  cover: {width: 300, height: 300, borderRadius: 16},
  text: {flex: 1, marginLeft: 48},
  title: {color: colors.text, fontSize: 32, fontWeight: '500'},
  sub: {color: colors.textDim, fontSize: 20, marginTop: 12},
  progress: {marginTop: 28},
  track: {
    height: 4,
    borderRadius: 2,
    backgroundColor: colors.surface,
    overflow: 'hidden',
  },
  bar: {height: 4, backgroundColor: colors.primary},
  time: {color: colors.textDim, fontSize: 14, marginTop: 8},
  from: {color: colors.primary, fontSize: 14, marginTop: 24},
});
