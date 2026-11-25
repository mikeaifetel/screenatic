import React from 'react';
import {Image, StyleSheet, Text, View} from 'react-native';
import {t} from './i18n';
import {colors} from './theme';

/** Idle screen, as on Android (HomeScreen.kt): how to connect, and the app's mark on the right. */
export const Home = ({deviceName, ip}: {deviceName: string; ip: string}) => (
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
    </View>
    <View style={styles.right}>
      <Image source={require('./assets/logo.png')} style={styles.logo} />
    </View>
  </View>
);

const styles = StyleSheet.create({
  // Vega lays out at 960x540 dp, like Android TV.
  root: {
    ...StyleSheet.absoluteFillObject,
    flexDirection: 'row',
    backgroundColor: colors.background,
    padding: 48,
  },
  left: {flex: 1, justifyContent: 'center', paddingRight: 48},
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
