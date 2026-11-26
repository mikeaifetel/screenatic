#!/usr/bin/env bash
# Sponsor reach report from Workers Analytics Engine (written by site/worker.js).
#
#   CF_ACCOUNT_ID=... CF_API_TOKEN=... scripts/sponsor-report.sh [days] [store|dev]
#
# CF_API_TOKEN: a Cloudflare API token with "Account Analytics: Read" (create it in the Cloudflare
# dashboard, My Profile -> API Tokens; keep it out of the repo). Defaults: last 30 days, store files.
# Output (tab-separated): day, item id, event (impression | scan), country, count.
# Analytics Engine keeps data for 3 months: export monthly reports.
set -euo pipefail
days="${1:-30}"
file="${2:-store}"
[[ "$days" =~ ^[0-9]+$ ]] || { echo "days must be a number" >&2; exit 2; }
[[ "$file" == store || "$file" == dev ]] || { echo "file must be store or dev" >&2; exit 2; }
: "${CF_ACCOUNT_ID:?set CF_ACCOUNT_ID}" "${CF_API_TOKEN:?set CF_API_TOKEN}"

sql="SELECT toDate(timestamp) AS day, index1 AS id, blob1 AS event, blob2 AS country,
       SUM(_sample_interval * double1) AS count
FROM screenatic_sponsor_events
WHERE timestamp > NOW() - INTERVAL '$days' DAY AND blob5 = '$file'
GROUP BY day, id, event, country
ORDER BY day, id, event, country
FORMAT TabSeparatedWithNames"

curl -sS --fail-with-body "https://api.cloudflare.com/client/v4/accounts/$CF_ACCOUNT_ID/analytics_engine/sql" \
  -H "Authorization: Bearer $CF_API_TOKEN" --data-binary "$sql"
