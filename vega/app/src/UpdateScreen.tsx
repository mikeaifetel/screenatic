import React from 'react';
import {Linking, Pressable, StyleSheet, Text, View} from 'react-native';
import {t} from './i18n';
import {colors} from './theme';

// The website links to the store pages. TODO: the app's Appstore page once it is listed.
const STORE_URL = 'https://screenatic.com/';

/** Shown when the remote config's minVegaBuild is above this build (see rules.ts). */
export const UpdateScreen = () => (
  <View style={styles.root}>
    <Text style={styles.title}>{t('update_title')}</Text>
    <Text style={styles.body}>{t('update_body')}</Text>
    <Pressable
      hasTVPreferredFocus
      onPress={() => Linking.openURL(STORE_URL).catch(() => {})}
      style={({focused}: any) => [
        styles.button,
        focused && styles.buttonFocused,
      ]}>
      <Text style={styles.buttonText}>{t('update_button')}</Text>
    </Pressable>
  </View>
);

const styles = StyleSheet.create({
  root: {
    ...StyleSheet.absoluteFillObject,
    backgroundColor: colors.background,
    alignItems: 'center',
    justifyContent: 'center',
    padding: 48,
  },
  title: {color: colors.text, fontSize: 32, fontWeight: '500'},
  body: {
    color: colors.text,
    fontSize: 16,
    textAlign: 'center',
    maxWidth: 640,
    marginTop: 16,
  },
  button: {
    marginTop: 32,
    paddingHorizontal: 24,
    paddingVertical: 10,
    borderRadius: 20,
    backgroundColor: colors.surface,
  },
  buttonFocused: {backgroundColor: colors.primary},
  buttonText: {color: colors.text, fontSize: 16, fontWeight: '500'},
});
