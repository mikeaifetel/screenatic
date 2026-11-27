import {AsyncStorage} from '@amazon-devices/react-native-kepler';

/**
 * Remote config shared with the Android app (docs/rules.json on screenatic.com), resolved the
 * same way as core/.../CastTimeRules.kt: "defaults", then the device's experiment arm, picked by
 * the same hash so a device stays in its arm. "minVegaBuild" (not Android's "minVersionCode")
 * gates this app: builds below it show the update screen.
 */
export const RULES_URL = __DEV__
  ? 'https://screenatic.com/rules-dev.json'
  : 'https://screenatic.com/rules.json';

/** The project site (rules, sponsor files, sponsor statistics). */
export const SITE = 'https://screenatic.com';

/** Keep equal to build-number in manifest.toml (= the Android versionCode). */
export const APP_BUILD = 5;

/** The TV app runs for days: refresh the rules periodically, not just at start. */
export const RULES_REFRESH_MS = 6 * 60 * 60 * 1000;

const CACHE_KEY = 'rules_json';

/** Tunable "ads for cast time" rules (seconds), defaults as on Android. */
export type CastTimeRules = {
  welcomeGrantSec: number;
  rewardSec: number;
  maxBalanceSec: number;
  /** Casting continues this long after the balance hits zero, so the user can watch an ad. */
  graceSec: number;
  /** Max rewarded ads per local day; 0 = unlimited. */
  dailyRewardCap: number;
  /** Show the display-ad slot on the idle screen. */
  /**
   * Sponsor slot on the idle screen. JSON key "sponsorSlot": builds up to 1.0.3 read "idleAds" as a
   * grey "Ad slot" placeholder, so "idleAds" must stay false (or absent).
   */
  sponsorSlot: boolean;
  /** Cast time is metered and rewarded ads offered. Off = free, no ad UI. */
  metering: boolean;
};

export const DEFAULT_RULES: CastTimeRules = {
  welcomeGrantSec: 30 * 60,
  rewardSec: 60 * 60,
  maxBalanceSec: 8 * 60 * 60,
  graceSec: 2 * 60,
  dailyRewardCap: 0,
  sponsorSlot: false,
  metering: false,
};

export type RulesAssignment = {
  rules: CastTimeRules;
  configVersion: number;
  experimentId: string | null;
  variant: string;
  minVegaBuild: number;
};

export const DEFAULT_ASSIGNMENT: RulesAssignment = {
  rules: DEFAULT_RULES,
  configVersion: 0,
  experimentId: null,
  variant: 'default',
  minVegaBuild: 0,
};

const num = (v: unknown): number | undefined =>
  typeof v === 'number' && Number.isFinite(v) ? v : undefined;

function overridden(r: CastTimeRules, o: any): CastTimeRules {
  if (!o || typeof o !== 'object') return r;
  const min = (k: string, fb: number) => (num(o[k]) !== undefined ? o[k] * 60 : fb);
  return {
    welcomeGrantSec: min('welcomeGrantMin', r.welcomeGrantSec),
    rewardSec: min('rewardMin', r.rewardSec),
    maxBalanceSec: min('maxBalanceMin', r.maxBalanceSec),
    graceSec: num(o.graceSec) ?? r.graceSec,
    dailyRewardCap: num(o.dailyRewardCap) ?? r.dailyRewardCap,
    sponsorSlot: typeof o.sponsorSlot === 'boolean' ? o.sponsorSlot : r.sponsorSlot,
    metering: typeof o.metering === 'boolean' ? o.metering : r.metering,
  };
}

/** Java's String.hashCode, so a device gets the same arm as the Android app's bucketing. */
function javaHash(s: string): number {
  let h = 0;
  // eslint-disable-next-line no-bitwise
  for (let i = 0; i < s.length; i++) h = (Math.imul(31, h) + s.charCodeAt(i)) | 0;
  return h;
}

export function resolve(config: any, deviceId: string): RulesAssignment {
  const base = overridden(DEFAULT_RULES, config?.defaults);
  const configVersion = num(config?.version) ?? 0;
  const minVegaBuild = num(config?.minVegaBuild) ?? 0;
  const plain = {...DEFAULT_ASSIGNMENT, rules: base, configVersion, minVegaBuild};
  const exp = config?.experiment;
  if (!exp || typeof exp.id !== 'string' || !Array.isArray(exp.variants)) return plain;
  const weights: number[] = exp.variants.map((v: any) => num(v?.weight) ?? 0);
  const total = weights.reduce((a, b) => a + b, 0);
  if (total <= 0) return plain;
  let bucket = ((javaHash(`${exp.id}:${deviceId}`) % total) + total) % total; // floorMod
  for (let i = 0; i < weights.length; i++) {
    if (bucket < weights[i]) {
      const v = exp.variants[i];
      return {
        rules: overridden(base, v?.rules),
        configVersion,
        experimentId: exp.id,
        variant: String(v?.name ?? i),
        minVegaBuild,
      };
    }
    bucket -= weights[i];
  }
  return plain;
}

/** The last good config (cached), or null. */
export async function loadCachedConfig(): Promise<any | null> {
  try {
    const s = await AsyncStorage.getItem(CACHE_KEY);
    return s ? JSON.parse(s) : null;
  } catch {
    return null;
  }
}

/** Fetches and caches the config. Null on any error: the cached rules stay. */
export async function fetchConfig(): Promise<any | null> {
  try {
    const ctl = new AbortController();
    const timeout = setTimeout(() => ctl.abort(), 8000);
    const res = await fetch(RULES_URL, {signal: ctl.signal});
    clearTimeout(timeout);
    if (!res.ok) return null;
    const text = await res.text();
    const json = JSON.parse(text);
    if (!json || typeof json !== 'object') return null;
    AsyncStorage.setItem(CACHE_KEY, text).catch(() => {});
    return json;
  } catch {
    return null;
  }
}
