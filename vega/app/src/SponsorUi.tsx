import React, {useEffect, useMemo, useState} from 'react';
import {Image, StyleSheet, Text, View} from 'react-native';
import qrcode from 'qrcode-generator';
import {t} from './i18n';
import {Sponsor} from './sponsors';
import {colors} from './theme';

/** Each item stays on screen at least this long (as on Android). */
const ROTATE_MS = 30_000;
const QR_SIZE = 132;
const IMAGE_HEIGHT = 96;

/** Weighted random pick, avoiding the one on screen when there's a choice. */
function pick(items: Sponsor[], previous: Sponsor | null): Sponsor {
  const pool = items.filter((s) => s.id !== previous?.id);
  const from = pool.length ? pool : items;
  let r = Math.floor(Math.random() * from.reduce((a, s) => a + s.weight, 0));
  for (const s of from) {
    r -= s.weight;
    if (r < 0) return s;
  }
  return from[from.length - 1];
}

/** QR code drawn with Views: one row per module line, dark runs merged. */
const QrCode = ({text}: {text: string}) => {
  const rows = useMemo(() => {
    try {
      const qr = qrcode(0, 'M');
      qr.addData(text);
      qr.make();
      const n = qr.getModuleCount();
      const out: {dark: boolean; len: number}[][] = [];
      for (let r = 0; r < n; r++) {
        const runs: {dark: boolean; len: number}[] = [];
        for (let c = 0; c < n; c++) {
          const dark = qr.isDark(r, c);
          const last = runs[runs.length - 1];
          if (last && last.dark === dark) last.len++;
          else runs.push({dark, len: 1});
        }
        out.push(runs);
      }
      return out;
    } catch {
      return null;
    }
  }, [text]);
  if (!rows) return null;
  const cell = QR_SIZE / rows.length;
  return (
    <View style={styles.qrBox}>
      {rows.map((runs, r) => (
        <View key={r} style={[styles.qrLine, {height: cell}]}>
          {runs.map((run, i) => (
            <View
              key={i}
              style={{
                width: run.len * cell,
                height: cell,
                backgroundColor: run.dark ? '#000' : '#fff',
              }}
            />
          ))}
        </View>
      ))}
    </View>
  );
};

/** Picture at IMAGE_HEIGHT, left-aligned like Android: width from the image's aspect ratio. */
const SponsorImage = ({uri, label}: {uri: string; label: string}) => {
  const [ratio, setRatio] = useState<number | null>(null);
  useEffect(() => {
    let alive = true;
    setRatio(null);
    Image.getSize(
      uri,
      (w, h) => {
        if (alive && w > 0 && h > 0) setRatio(w / h);
      },
      () => {},
    );
    return () => {
      alive = false;
    };
  }, [uri]);
  if (!ratio) return null;
  return (
    <Image
      source={{uri}}
      style={[styles.image, {width: IMAGE_HEIGHT * ratio}]}
      resizeMode="contain"
      accessibilityLabel={label}
    />
  );
};

/**
 * The idle-screen sponsor slot, as on Android (SponsorSlot.kt): label, optional picture, title,
 * text and a QR code. Not focusable and not pressable: the QR code is the only way out.
 */
export const SponsorSlot = ({
  items,
  onShown,
}: {
  items: Sponsor[];
  onShown: (id: string) => void;
}) => {
  const [current, setCurrent] = useState<Sponsor | null>(null);
  const [turn, setTurn] = useState(0);
  useEffect(() => {
    if (!items.length) return;
    const next = () => {
      setCurrent((prev) => pick(items, prev));
      setTurn((n) => n + 1);
    };
    next();
    const timer = setInterval(next, ROTATE_MS);
    return () => clearInterval(timer);
  }, [items]);
  const s = current && items.some((i) => i.id === current.id) ? current : items[0];
  const shownId = s?.id;
  // An impression: the item stayed on screen for a second (IAB/MRC display viewability).
  useEffect(() => {
    if (!shownId) return;
    const timer = setTimeout(() => onShown(shownId), 1000);
    return () => clearTimeout(timer);
  }, [shownId, turn, onShown]);
  if (!s) return null;
  return (
    <View style={styles.card} focusable={false}>
      <Text style={[styles.label, s.house ? styles.labelHouse : styles.labelSponsored]}>
        {t(s.house ? 'house_label' : 'sponsored_label')}
      </Text>
      {s.image ? (
<SponsorImage uri={s.image} label={s.title} />
      ) : null}
      <Text style={styles.title} numberOfLines={2}>
        {s.title}
      </Text>
      {s.body ? (
        <Text style={styles.body} numberOfLines={4}>
          {s.body}
        </Text>
      ) : null}
      <View style={styles.spacer} />
      {s.qr ? (
        <View style={styles.qrRow}>
          <QrCode text={s.qr} />
          <Text style={styles.scan}>{t('scan_qr')}</Text>
        </View>
      ) : null}
    </View>
  );
};

const styles = StyleSheet.create({
  card: {
    flex: 1,
    borderRadius: 24,
    backgroundColor: colors.surface,
    padding: 28,
    gap: 14,
  },
  label: {
    alignSelf: 'flex-start',
    color: '#fff',
    fontSize: 14,
    fontWeight: '500',
    paddingHorizontal: 10,
    paddingVertical: 4,
    borderRadius: 8,
    overflow: 'hidden',
  },
  labelHouse: {backgroundColor: 'rgba(255,255,255,0.4)'},
  labelSponsored: {backgroundColor: colors.primary},
  image: {height: IMAGE_HEIGHT, maxWidth: '100%', alignSelf: 'flex-start'},
  title: {color: colors.text, fontSize: 24, fontWeight: '500'},
  body: {color: colors.textDim, fontSize: 14, lineHeight: 20},
  spacer: {flex: 1},
  qrRow: {flexDirection: 'row', alignItems: 'center', gap: 16},
  qrLine: {flexDirection: 'row'},
  qrBox: {backgroundColor: '#fff', padding: 8, borderRadius: 8, flexShrink: 0},
  scan: {color: colors.text, fontSize: 14, flexShrink: 1},
});
