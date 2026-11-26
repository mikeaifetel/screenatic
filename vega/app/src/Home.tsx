import React from 'react';
import {Image, StyleSheet, Text, View} from 'react-native';
import {CastTimeLine, DevTools, WatchAdButton} from './CastTimeUi';
import {CastTimeState, devResetBalance, setDevUnlimited} from './castTime';
import {t} from './i18n';
import {SponsorSlot} from './SponsorUi';
import {useSponsors} from './sponsors';
import {countShown} from './sponsorStats';
import {colors} from './theme';

/**
 * Idle screen, as on Android (HomeScreen.kt): how to connect, cast time and the ad button when
 * metered, dev switches in debug builds, and the app's mark (or the sponsor slot) on the right.
 */
export const Home = ({
  deviceName,
  ip,
  castTime,
  onWatchAd,
  hidden,
}: {
  deviceName: string;
  ip: string;
  castTime: CastTimeState;
  onWatchAd: () => void;
  /** Covered by the ad placeholder: its buttons take the focus. */
  hidden?: boolean;
}) => {
  // Sponsor slot only when the rules turn it on and the sponsor file has an item for this
  // device; otherwise (and offline) the app's mark.
  const sponsors = useSponsors(castTime.sponsorSlot);
  return (
  <View style={styles.root}>
    <View style={styles.left}>
      <Text style={styles.title}>{t('home_title')}</Text>
      <Text style={styles.name}>{deviceName}</Text>
      <View style={styles.hint}>
        <Text style={styles.label}>{t('hint_apple_label')}</Text>
        <Text style={styles.body}>{t('hint_apple')}</Text>
      </View>
      <View style={styles.hint2}>
        <Text style={styles.label}>{t('hint_other_label')}</Text>
        <Text style={styles.body}>{t('hint_other')}</Text>
      </View>
      <Text style={styles.body}>{t('hint_choose', deviceName)}</Text>
      {ip ? <Text style={styles.small}>{t('home_ip', ip)}</Text> : null}
      <CastTimeLine s={castTime} />
      {/* Free (metering off): no balance, no ad button. */}
      {!castTime.unlimited ? (
        <View style={styles.ad}>
          <WatchAdButton s={castTime} onWatchAd={onWatchAd} preferred={!hidden} />
        </View>
      ) : null}
      {__DEV__ ? (
        <DevTools
          s={castTime}
          onSetUnlimited={setDevUnlimited}
          onReset={devResetBalance}
          preferred={!hidden && castTime.unlimited}
        />
      ) : null}
    </View>
    {sponsors.length ? (
      <View style={styles.slot}>
        <SponsorSlot items={sponsors} onShown={countShown} />
      </View>
    ) : (
      <View style={styles.right}>
        <Image source={require('./assets/logo.png')} style={styles.logo} />
      </View>
    )}
  </View>
  );
};

const styles = StyleSheet.create({
  // Vega lays out at 960x540 dp, like Android TV.
  root: {
    ...StyleSheet.absoluteFillObject,
    flexDirection: 'row',
    backgroundColor: colors.background,
    padding: 48,
  },
  left: {flex: 1, justifyContent: 'center', paddingRight: 48},
  slot: {flex: 1},
  ad: {marginTop: 16},
  right: {flex: 0.8, alignItems: 'center', justifyContent: 'center'},
  title: {color: colors.text, fontSize: 32, fontWeight: '500'},
  name: {color: colors.primary, fontSize: 24, marginTop: 8},
  hint: {marginTop: 20},
  hint2: {marginTop: 12, marginBottom: 10},
  label: {color: colors.primary, fontSize: 14, fontWeight: '500'},
  body: {color: colors.text, fontSize: 14, lineHeight: 20},
  small: {color: colors.textDim, fontSize: 12, marginTop: 4},
  logo: {width: 300, height: 300, borderRadius: 64},
});
