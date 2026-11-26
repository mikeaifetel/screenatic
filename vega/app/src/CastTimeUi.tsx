import React from 'react';
import {Pressable, StyleSheet, Text, View} from 'react-native';
import {CastTimeState, formatDuration} from './castTime';
import {t} from './i18n';
import {colors} from './theme';

/** Cast-time UI, as on Android (HomeScreen.kt / DevRewardedAd.kt). */

/** "Cast time left: 12:34" (nothing when casting is free). */
export const CastTimeLine = ({s}: {s: CastTimeState}) => {
  if (!s.devUnlimited && s.unlimited) return null;
  return (
    <Text style={styles.line}>
      {s.devUnlimited
        ? t('time_dev_unlimited')
        : t('time_left', formatDuration(s.remainingSec))}
    </Text>
  );
};

const Button = ({
  label,
  onPress,
  preferred,
  outlined,
}: {
  label: string;
  onPress: () => void;
  preferred?: boolean;
  outlined?: boolean;
}) => (
  <Pressable
    hasTVPreferredFocus={preferred}
    onPress={onPress}
    style={({focused}: any) => [
      styles.button,
      outlined && styles.outlined,
      focused && styles.focused,
    ]}>
    <Text style={styles.buttonText}>{label}</Text>
  </Pressable>
);

/** "Watch an ad · +60 min", or the daily-cap note. */
export const WatchAdButton = ({
  s,
  onWatchAd,
  preferred,
}: {
  s: CastTimeState;
  onWatchAd: () => void;
  preferred?: boolean;
}) =>
  s.canEarnReward ? (
    <Button
      label={t('watch_ad', Math.round(s.rewardSec / 60))}
      onPress={onWatchAd}
      preferred={preferred}
    />
  ) : (
    <Text style={styles.note}>{t('reward_cap_reached')}</Text>
  );

/** Debug builds: unlimited switch and balance reset. */
export const DevTools = ({
  s,
  onSetUnlimited,
  onReset,
  preferred,
}: {
  s: CastTimeState;
  onSetUnlimited: (u: boolean) => void;
  onReset: () => void;
  preferred?: boolean;
}) => (
  <View style={styles.row}>
    <Button
      outlined
      preferred={preferred}
      label={t(s.devUnlimited ? 'dev_unlimited_on' : 'dev_unlimited_off')}
      onPress={() => onSetUnlimited(!s.devUnlimited)}
    />
    <Button outlined label={t('dev_reset_balance')} onPress={onReset} />
  </View>
);

/**
 * On top of a running session when the balance is used up. Not focusable: over the mirror or a
 * video the remote's OK opens the ad (App.tsx), so the button only shows what OK does.
 */
export const GraceBanner = ({s}: {s: CastTimeState}) => {
  if (s.graceLeftSec === null) return null;
  return (
    <View style={styles.bannerWrap} pointerEvents="none">
      <View style={styles.banner}>
        <Text style={styles.bannerText}>
          {t('grace_banner', formatDuration(s.graceLeftSec))}
        </Text>
        {s.canEarnReward ? (
          <View style={[styles.button, styles.focused]}>
            <Text style={styles.buttonText}>
              {t('watch_ad', Math.round(s.rewardSec / 60))}
            </Text>
          </View>
        ) : (
          <Text style={styles.bannerText}>{t('reward_cap_reached')}</Text>
        )}
      </View>
    </View>
  );
};

/**
 * Placeholder ad, as Android's DevRewardedAd: a dialog standing in for a 15–30 s rewarded video,
 * so the cast-time loop can be tested end to end before an ad source exists.
 */
export const AdPlaceholder = ({
  onReward,
  onClose,
}: {
  onReward: () => void;
  onClose: () => void;
}) => (
  <View style={styles.adRoot}>
    <View style={styles.adBox}>
      <Text style={styles.adTitle}>[vega] Rewarded ad placeholder</Text>
      <Text style={styles.adBody}>A real 15–30 s rewarded video plays here.</Text>
      <View style={styles.row}>
        <Button
          preferred
          label="Finish ad"
          onPress={() => {
            onReward();
            onClose();
          }}
        />
        <Button outlined label="Skip" onPress={onClose} />
      </View>
    </View>
  </View>
);

const styles = StyleSheet.create({
  line: {color: colors.text, fontSize: 18, fontWeight: '500', marginTop: 24},
  row: {flexDirection: 'row', alignItems: 'center', gap: 12, marginTop: 16},
  button: {
    alignSelf: 'flex-start',
    paddingHorizontal: 20,
    paddingVertical: 8,
    borderRadius: 20,
    backgroundColor: colors.surface,
    borderWidth: 1,
    borderColor: colors.surface,
  },
  outlined: {backgroundColor: 'transparent', borderColor: colors.textDim},
  focused: {backgroundColor: colors.primary, borderColor: colors.primary},
  buttonText: {color: colors.text, fontSize: 14, fontWeight: '500'},
  note: {color: colors.text, fontSize: 14, marginTop: 16},
  bannerWrap: {
    ...StyleSheet.absoluteFillObject,
    justifyContent: 'flex-end',
    alignItems: 'center',
    padding: 24,
  },
  banner: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 16,
    padding: 16,
    backgroundColor: 'rgba(0,0,0,0.8)',
  },
  bannerText: {color: '#fff', fontSize: 14, flexShrink: 1},
  adRoot: {
    ...StyleSheet.absoluteFillObject,
    backgroundColor: 'rgba(0,0,0,0.6)',
    alignItems: 'center',
    justifyContent: 'center',
  },
  adBox: {
    backgroundColor: colors.surface,
    padding: 32,
    borderRadius: 16,
    maxWidth: 560,
  },
  adTitle: {color: colors.text, fontSize: 20, fontWeight: '500'},
  adBody: {color: colors.text, fontSize: 14, marginTop: 12},
});
