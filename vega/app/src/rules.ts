/**
 * Remote config shared with the Android app (docs/rules.json on screenatic.com, see
 * core/.../CastTimeRules.kt). Vega reads only "minVegaBuild": builds below it show the update
 * screen. Cast-time metering needs rewarded ads, which the Vega app doesn't have, so casting stays
 * free here whatever "metering" says.
 */
export const RULES_URL = __DEV__
  ? 'https://screenatic.com/rules-dev.json'
  : 'https://screenatic.com/rules.json';

/** Keep equal to build-number in manifest.toml. */
export const APP_BUILD = 1;

/** True when this build is too old. Network errors and bad JSON never block casting. */
export async function updateRequired(): Promise<boolean> {
  try {
    const ctl = new AbortController();
    const timeout = setTimeout(() => ctl.abort(), 8000);
    const res = await fetch(RULES_URL, {signal: ctl.signal});
    clearTimeout(timeout);
    if (!res.ok) return false;
    const json = await res.json();
    const min = Number(json?.minVegaBuild ?? 0);
    return Number.isFinite(min) && APP_BUILD < min;
  } catch {
    return false;
  }
}
