import {useEffect, useState} from 'react';
import {AsyncStorage} from '@amazon-devices/react-native-kepler';
import {language, locale} from './i18n';
import {SITE} from './rules';
import {flushStats} from './sponsorStats';

/**
 * Self-run sponsor slot on the idle home screen, as on Android (sponsor/Sponsors.kt): the same
 * JSON on screenatic.com (docs/sponsors.json; debug builds read sponsors-dev.json), fetched whole
 * and filtered on the device by language, region and date. Nothing is sent to the server. See
 * Sponsors.kt for the format and the text-length rules. Anything invalid is dropped; no valid
 * item = no slot.
 */
const ID = /^[a-z0-9][a-z0-9-]{0,39}$/;

export const SPONSORS_URL = __DEV__
  ? 'https://screenatic.com/sponsors-dev.json'
  : 'https://screenatic.com/sponsors.json';

const CACHE_KEY = 'sponsors_json';
const CACHE_AT_KEY = 'sponsors_at';
const MAX_CHARS = 64 * 1024;
/** On-screen caps, as on Android; the card also ellipsizes at 2 / 4 lines. */
const MAX_TITLE = 80;
const MAX_BODY = 300;
export const SPONSORS_REFRESH_MS = 6 * 60 * 60 * 1000;

export type Sponsor = {
  id: string;
  house: boolean;
  title: string;
  body?: string;
  qr?: string;
  image?: string;
  weight: number;
};

/** Fallback country: the system locale's region ("en-US" -> "US"), often en-US anywhere. */
function localeRegion(): string {
  const parts = locale.replace(/_/g, '-').split('-');
  const r = parts.slice(1).find((p) => /^[A-Za-z]{2}$/.test(p));
  return (r ?? '').toUpperCase();
}

function todayUtc(): string {
  return new Date().toISOString().slice(0, 10);
}

function localized(map: any, lang: string): string | undefined {
  if (!map || typeof map !== 'object') return undefined;
  const base = lang.split('-')[0];
  for (const k of [lang, base, 'en']) {
    if (typeof map[k] === 'string' && map[k].trim()) return map[k];
  }
  return Object.values(map).find((v) => typeof v === 'string' && v.trim()) as string | undefined;
}

const strings = (v: any): string[] | undefined =>
  Array.isArray(v) ? v.filter((x) => typeof x === 'string') : undefined;

function toSponsor(o: any, lang: string, reg: string, today: string): Sponsor | null {
  if (!o || typeof o !== 'object') return null;
  if (typeof o.id !== 'string' || !ID.test(o.id)) return null;
  if (o.kind !== 'house' && o.kind !== 'sponsored') return null;
  const langs = strings(o.languages);
  if (langs && !langs.includes(lang)) return null;
  const regions = strings(o.regions);
  if (regions && !regions.some((r) => r.toUpperCase() === reg)) return null;
  const excluded = strings(o.excludeRegions);
  if (excluded && excluded.some((r) => r.toUpperCase() === reg)) return null;
  if (typeof o.start === 'string' && o.start && today < o.start) return null;
  if (typeof o.end === 'string' && o.end && today > o.end) return null;
  const title = localized(o.title, lang)?.slice(0, MAX_TITLE);
  if (!title) return null;
  const weight = typeof o.weight === 'number' ? Math.max(0, Math.floor(o.weight)) : 1;
  if (weight === 0) return null;
  const https = (v: any, max = 2048) =>
    typeof v === 'string' && v.startsWith('https://') && v.length <= max ? v : undefined;
  return {
    id: o.id,
    house: o.kind === 'house',
    title,
    body: localized(o.body, lang)?.slice(0, MAX_BODY),
    // The QR code is screenatic.com/c/{id}: the site counts the scan and redirects to "qr".
    qr: https(o.qr, 300) ? `${SITE}/c/${o.id}` : undefined,
    image: https(o.image),
    weight,
  };
}

type SponsorFile = {items: any[]; country?: string};

/**
 * Raw items; throws on a malformed file. "country" is added by the site's worker
 * (site/worker.js): the country of the connection, preferred over the locale's region.
 */
function parse(text: string): SponsorFile {
  const root = JSON.parse(text);
  if (!root || !Array.isArray(root.items)) throw new Error('no items');
  const c = root.country;
  return {items: root.items, country: typeof c === 'string' && /^[A-Z]{2}$/.test(c) ? c : undefined};
}

function eligible({items, country}: SponsorFile): Sponsor[] {
  const reg = country ?? localeRegion();
  const today = todayUtc();
  return items
    .map((o) => {
      try {
        return toSponsor(o, language, reg, today);
      } catch {
        return null;
      }
    })
    .filter((s): s is Sponsor => s !== null);
}

async function fetchText(): Promise<string | null> {
  try {
    const ctl = new AbortController();
    const timeout = setTimeout(() => ctl.abort(), 8000);
    const res = await fetch(SPONSORS_URL, {signal: ctl.signal});
    clearTimeout(timeout);
    if (!res.ok) return null;
    const text = await res.text();
    return text.length <= MAX_CHARS ? text : null;
  } catch {
    return null;
  }
}

/** Cached items for this device, refreshed when the cache is older than SPONSORS_REFRESH_MS. */
export async function loadSponsors(): Promise<Sponsor[]> {
  let text: string | null = null;
  let at = 0;
  try {
    text = await AsyncStorage.getItem(CACHE_KEY);
    at = Number((await AsyncStorage.getItem(CACHE_AT_KEY)) ?? 0) || 0;
  } catch {
    // no cache
  }
  const age = Date.now() - at;
  if (!text || age < 0 || age > SPONSORS_REFRESH_MS) {
    const fresh = await fetchText();
    if (fresh) {
      try {
        parse(fresh);
        text = fresh;
        AsyncStorage.setItem(CACHE_KEY, fresh).catch(() => {});
        AsyncStorage.setItem(CACHE_AT_KEY, String(Date.now())).catch(() => {});
      } catch {
        // bad file: keep the cached one
      }
    }
  }
  if (!text) return [];
  try {
    return eligible(parse(text));
  } catch {
    return [];
  }
}

/**
 * The slot's items while `enabled` (rules sponsorSlot) and the app is in front; [] otherwise.
 * Vega closes the app when it leaves the foreground, so a mounted home screen is in front.
 */
export function useSponsors(enabled: boolean): Sponsor[] {
  const [items, setItems] = useState<Sponsor[]>([]);
  useEffect(() => {
    if (!enabled) {
      setItems([]);
      return;
    }
    let alive = true;
    const load = async () => {
      await flushStats();
      const s = await loadSponsors();
      if (alive) setItems(s);
    };
    load();
    const timer = setInterval(load, SPONSORS_REFRESH_MS);
    return () => {
      alive = false;
      clearInterval(timer);
    };
  }, [enabled]);
  return items;
}
