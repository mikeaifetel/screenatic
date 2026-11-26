// screenatic.com: static site from docs/ (Workers static assets). This script runs only for the
// paths in run_worker_first (wrangler.jsonc); everything else is served as a plain asset.
//
//   /sponsors.json, /sponsors-dev.json  the sponsor file plus "country": the country of the
//                                       connection (Cloudflare, from the IP), so the apps can pick
//                                       items by country. Not cached: it differs per country.
//   POST /px                            impression counts the apps batch up (see below).
//   /c/{id}                             a QR scan: counted, then redirected to the item's "qr" URL.
//
// Reach statistics go to Workers Analytics Engine (binding SPONSOR_STATS) as aggregate data points:
// item id, event, country, app, build, file. No IP, no device identifier, nothing else is stored.
// scripts/sponsor-report.sh queries them.

const SPONSOR_FILES = {'/sponsors.json': 'store', '/sponsors-dev.json': 'dev'};
const ID = /^[a-z0-9][a-z0-9-]{0,39}$/;
const MAX_PX_BYTES = 4096;

const noStore = {'cache-control': 'private, no-store'};

/** Country of the connection, or "" ("XX" = unknown, "T1" = Tor). */
function country(request) {
  const c = request.cf?.country;
  return typeof c === 'string' && /^[A-Z]{2}$/.test(c) && c !== 'XX' ? c : '';
}

/** One aggregate data point; never fails the request. */
function record(env, id, event, ctry, app, build, file, n) {
  try {
    env.SPONSOR_STATS?.writeDataPoint({
      indexes: [id],
      blobs: [event, ctry, app, String(build), file],
      doubles: [n],
    });
  } catch {
    // statistics are best effort
  }
}

async function sponsorFile(env, request, path) {
  const res = await env.ASSETS.fetch(new Request(new URL(path, request.url)));
  if (!res.ok) return null;
  try {
    return await res.json();
  } catch {
    return null;
  }
}

async function serveSponsors(request, env, path) {
  const data = await sponsorFile(env, request, path);
  if (!data) return new Response('bad sponsor file', {status: 500});
  data.country = country(request) || null;
  return new Response(JSON.stringify(data), {
    headers: {'content-type': 'application/json; charset=utf-8', ...noStore},
  });
}

/**
 * Body: {"file": "store"|"dev", "app": "android"|"vega", "build": 5, "counts": {"house-beta": 12}}
 * Each count = how many times the item was on screen for at least a second since the last report.
 */
async function impressions(request, env) {
  if (request.method !== 'POST') return new Response(null, {status: 405, headers: {allow: 'POST'}});
  const text = await request.text();
  if (text.length > MAX_PX_BYTES) return new Response(null, {status: 413});
  let b;
  try {
    b = JSON.parse(text);
  } catch {
    return new Response(null, {status: 400});
  }
  const ok =
    b && (b.file === 'store' || b.file === 'dev') && (b.app === 'android' || b.app === 'vega') &&
    Number.isInteger(b.build) && b.build >= 0 && b.build < 1e6 &&
    b.counts && typeof b.counts === 'object' && !Array.isArray(b.counts);
  const entries = ok ? Object.entries(b.counts) : [];
  if (!ok || entries.length > 50) return new Response(null, {status: 400});
  const ctry = country(request);
  for (const [id, n] of entries) {
    if (ID.test(id) && Number.isInteger(n) && n > 0 && n <= 10000) {
      record(env, id, 'impression', ctry, b.app, b.build, b.file, n);
    }
  }
  return new Response(null, {status: 204, headers: noStore});
}

/** /c/{id}: the item's QR target, looked up in the store file, then the dev file. */
async function scan(request, env, id) {
  if (ID.test(id)) {
    for (const [path, file] of Object.entries(SPONSOR_FILES)) {
      const data = await sponsorFile(env, request, path);
      const item = Array.isArray(data?.items) ? data.items.find((o) => o?.id === id) : null;
      if (item && typeof item.qr === 'string' && item.qr.startsWith('https://')) {
        record(env, id, 'scan', country(request), '', 0, file, 1);
        return new Response(null, {status: 302, headers: {location: item.qr, ...noStore}});
      }
    }
  }
  return Response.redirect(new URL('/', request.url).toString(), 302);
}

export default {
  async fetch(request, env) {
    const {pathname} = new URL(request.url);
    if (pathname in SPONSOR_FILES) return serveSponsors(request, env, pathname);
    if (pathname === '/px') return impressions(request, env);
    if (pathname.startsWith('/c/')) return scan(request, env, pathname.slice(3));
    return env.ASSETS.fetch(request);
  },
};
