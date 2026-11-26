import {AsyncStorage} from '@amazon-devices/react-native-kepler';
import {APP_BUILD, SITE} from './rules';

/**
 * Sponsor reach, as on Android (sponsor/SponsorStats.kt): how many times each item was on screen
 * for at least a second, counted on the device and reported in one batch with each sponsor-file
 * refresh to screenatic.com/px (site/worker.js), which keeps only aggregate counts. No device
 * identifier is sent. QR scans are counted by the site's /c/{id} redirect.
 */
const KEY = 'sponsor_stats';
let counts: Record<string, number> | null = null;

async function loaded(): Promise<Record<string, number>> {
  if (counts) return counts;
  try {
    const s = await AsyncStorage.getItem(KEY);
    counts = s ? JSON.parse(s) : {};
  } catch {
    counts = {};
  }
  return counts ?? {};
}

function save() {
  AsyncStorage.setItem(KEY, JSON.stringify(counts ?? {})).catch(() => {});
}

export async function countShown(id: string) {
  const c = await loaded();
  c[id] = (c[id] ?? 0) + 1;
  save();
}

/** Sends the counts so far; they're kept (and sent next time) if the report fails. */
export async function flushStats() {
  const c = await loaded();
  const sent = {...c};
  if (!Object.keys(sent).length) return;
  try {
    const ctl = new AbortController();
    const timeout = setTimeout(() => ctl.abort(), 8000);
    const res = await fetch(`${SITE}/px`, {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({
        file: __DEV__ ? 'dev' : 'store',
        app: 'vega',
        build: APP_BUILD,
        counts: sent,
      }),
      signal: ctl.signal,
    });
    clearTimeout(timeout);
    if (res.status !== 204) return;
  } catch {
    return;
  }
  // Keep what was counted while the report was on its way.
  for (const [id, n] of Object.entries(sent)) {
    const left = (c[id] ?? 0) - n;
    if (left > 0) c[id] = left;
    else delete c[id];
  }
  save();
}
