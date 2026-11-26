import {useEffect, useState} from 'react';
import {AsyncStorage} from '@amazon-devices/react-native-kepler';
import {ProbeNative} from '@amazon-devices/probenative';
import {emitLocalEvent} from './airplay';
import {
  APP_BUILD,
  DEFAULT_ASSIGNMENT,
  RULES_REFRESH_MS,
  RulesAssignment,
  fetchConfig,
  loadCachedConfig,
  resolve,
} from './rules';

/**
 * Cast time ("ads for time"), the Android app's CastTimeLedger + SessionManager metering:
 * a local balance (welcome grant on first launch, + rewardMin per rewarded ad up to
 * maxBalanceMin, at most dailyRewardCap ads a day), debited every second while a sender is
 * connected and metering is on. At zero, casting goes on for graceSec with a banner, then the
 * sender is dropped. Debug builds: a home-screen switch overrides "metering" (default unlimited).
 *
 * TODO(server): as on Android, the balance is a local cache; the source of truth must be the
 * backend, crediting rewards from the ad network's server-side verification.
 */
export type CastTimeState = {
  remainingSec: number;
  /** Seconds left in the grace window, non-null once the balance is empty during a session. */
  graceLeftSec: number | null;
  /** False once today's rewarded-ad cap is reached. */
  canEarnReward: boolean;
  rewardSec: number;
  idleAds: boolean;
  /** Not metered (remote rules say free, or the dev switch): hide all cast-time / ad UI. */
  unlimited: boolean;
  /** Debug builds: the "unlimited" dev switch is on. */
  devUnlimited: boolean;
  /** This build is below the remote minVegaBuild: update screen, receivers off. */
  updateRequired: boolean;
};

type Ledger = {remaining: number; welcomed: boolean; rewardDay: number; rewardsToday: number};

const LEDGER_KEY = 'cast_time_ledger';
const DEV_UNLIMITED_KEY = 'dev_unlimited';

let ledger: Ledger = {remaining: 0, welcomed: false, rewardDay: -1, rewardsToday: 0};
let assignment: RulesAssignment = DEFAULT_ASSIGNMENT;
/** Debug builds: the dev switch's metering (overrides the rules); null in release builds. */
let devMetering: boolean | null = null;
let grace: number | null = null;
let sessionActive = false;
let meterTimer: ReturnType<typeof setInterval> | null = null;
let saveTimer: ReturnType<typeof setTimeout> | null = null;
let started = false;

const listeners = new Set<(s: CastTimeState) => void>();

/** Local calendar day number. */
function localDay(): number {
  const now = Date.now();
  return Math.floor((now - new Date(now).getTimezoneOffset() * 60000) / 86400000);
}

const rewardsToday = () => (ledger.rewardDay === localDay() ? ledger.rewardsToday : 0);
const meteringOn = () => devMetering ?? assignment.rules.metering;

function snapshot(): CastTimeState {
  const r = assignment.rules;
  return {
    remainingSec: ledger.remaining,
    graceLeftSec: grace,
    canEarnReward: r.dailyRewardCap <= 0 || rewardsToday() < r.dailyRewardCap,
    rewardSec: r.rewardSec,
    idleAds: r.idleAds,
    unlimited: !meteringOn(),
    devUnlimited: devMetering === false,
    updateRequired: assignment.minVegaBuild > APP_BUILD,
  };
}

let current = snapshot();
function publish() {
  current = snapshot();
  listeners.forEach((l) => l(current));
  updateMeter();
}

function save(now = false) {
  const write = () => {
    saveTimer = null;
    AsyncStorage.setItem(LEDGER_KEY, JSON.stringify(ledger)).catch(() => {});
  };
  if (now) {
    if (saveTimer) clearTimeout(saveTimer);
    write();
  } else if (!saveTimer) {
    saveTimer = setTimeout(write, 5000); // the per-second debit is written every 5 s
  }
}

function applyConfig(config: any) {
  if (!config) return;
  try {
    assignment = resolve(config, deviceId());
  } catch {
    return; // bad config: keep the old rules
  }
  publish();
}

function deviceId(): string {
  try {
    return JSON.parse(ProbeNative.status())?.id ?? '';
  } catch {
    return '';
  }
}

/** Once at app start: cached rules and balance, welcome grant, then the periodic rules refresh. */
export async function startCastTime(): Promise<void> {
  if (started) return;
  started = true;
  try {
    const s = await AsyncStorage.getItem(LEDGER_KEY);
    if (s) ledger = {...ledger, ...JSON.parse(s)};
  } catch {
    // keep defaults
  }
  if (__DEV__) {
    try {
      devMetering = (await AsyncStorage.getItem(DEV_UNLIMITED_KEY)) === '0';
    } catch {
      devMetering = false;
    }
  }
  applyConfig(await loadCachedConfig());
  if (!ledger.welcomed) {
    ledger = {...ledger, welcomed: true, remaining: assignment.rules.welcomeGrantSec};
    save(true);
  }
  publish();
  const refresh = async () => applyConfig(await fetchConfig());
  refresh();
  setInterval(refresh, RULES_REFRESH_MS);
}

/** A sender is connected (mirror, video, music, photos): meter while metering is on. */
export function setSessionActive(active: boolean) {
  if (sessionActive === active) return;
  sessionActive = active;
  updateMeter();
}

function updateMeter() {
  const meter = sessionActive && meteringOn();
  if (meter && !meterTimer) {
    meterTimer = setInterval(tick, 1000);
  } else if (!meter && meterTimer) {
    clearInterval(meterTimer);
    meterTimer = null;
    if (grace !== null) {
      grace = null;
      publish();
    }
    save(true);
  }
}

function tick() {
  ledger = {...ledger, remaining: Math.max(0, ledger.remaining - 1)};
  save();
  if (ledger.remaining > 0) {
    grace = null;
  } else {
    const g = (grace ?? assignment.rules.graceSec) - 1;
    if (g > 0) {
      grace = g;
    } else {
      grace = null;
      dropSenders();
    }
  }
  publish();
}

/** Cast time is up: drop the AirPlay sender (native) and close a DLNA item (UrlPlayer). */
function dropSenders() {
  try {
    ProbeNative.msseAttach('cmd:disconnect');
  } catch {
    // ignore
  }
  emitLocalEvent({type: 'timeup'});
}

/** After a rewarded ad pays out. False when today's cap is reached. */
export function creditReward(): boolean {
  const r = assignment.rules;
  const today = localDay();
  const count = rewardsToday();
  if (r.dailyRewardCap > 0 && count >= r.dailyRewardCap) return false;
  ledger = {
    ...ledger,
    remaining: Math.min(ledger.remaining + r.rewardSec, r.maxBalanceSec),
    rewardDay: today,
    rewardsToday: count + 1,
  };
  grace = null;
  save(true);
  // TODO(analytics): log the reward with assignment.experimentId / variant.
  publish();
  return true;
}

/** Debug builds: the "unlimited cast time" switch, remembered across restarts. */
export function setDevUnlimited(unlimited: boolean) {
  if (!__DEV__) return;
  devMetering = !unlimited;
  AsyncStorage.setItem(DEV_UNLIMITED_KEY, unlimited ? '1' : '0').catch(() => {});
  publish();
}

/** Debug builds: zero the balance and today's ad count to test running out / the ad flow. */
export function devResetBalance() {
  if (!__DEV__) return;
  ledger = {...ledger, remaining: 0, rewardsToday: 0};
  save(true);
  publish();
}

export function useCastTime(): CastTimeState {
  const [s, setS] = useState(current);
  useEffect(() => {
    listeners.add(setS);
    setS(current);
    return () => {
      listeners.delete(setS);
    };
  }, []);
  return s;
}

/** "1:02:03" / "4:05", as on Android. */
export function formatDuration(sec: number): string {
  const h = Math.floor(sec / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = Math.floor(sec % 60);
  const p = (n: number) => String(n).padStart(2, '0');
  return h > 0 ? `${h}:${p(m)}:${p(s)}` : `${m}:${p(s)}`;
}
