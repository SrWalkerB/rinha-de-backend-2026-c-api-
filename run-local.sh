#!/usr/bin/env bash
# Local end-to-end runner (Linux / macOS).
#   ./run-local.sh           full k6 load test
#   ./run-local.sh --smoke   quick smoke
#   ./run-local.sh --down    tear down
#   ./run-local.sh --no-build
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RINHA="$(cd "$ROOT/../rinha-de-backend-2026" && pwd)"
TESTDIR="$RINHA/test"
COMPOSE="$ROOT/docker-compose.yml"
SMOKE=0; BUILD="--build"
for a in "$@"; do
  case "$a" in
    --smoke) SMOKE=1 ;;
    --down)  docker compose -f "$COMPOSE" down; exit 0 ;;
    --no-build) BUILD="" ;;
  esac
done

# 1. Stage dataset into build context.
mkdir -p "$ROOT/resources"
if [ ! -f "$ROOT/resources/references.json.gz" ] || \
   [ "$(stat -c%s "$RINHA/resources/references.json.gz")" != "$(stat -c%s "$ROOT/resources/references.json.gz" 2>/dev/null || echo 0)" ]; then
  echo "Staging references.json.gz ..."
  cp "$RINHA/resources/references.json.gz" "$ROOT/resources/references.json.gz"
fi

# 2. Up.
docker compose -f "$COMPOSE" up -d $BUILD

# 3. Wait for /ready.
echo "Waiting for http://localhost:9999/ready ..."
ok=0
for _ in $(seq 1 20); do
  if curl -fs -o /dev/null --max-time 3 http://localhost:9999/ready; then ok=1; break; fi
  sleep 3
done
[ "$ok" = 1 ] || { echo "API never ready"; docker compose -f "$COMPOSE" logs --tail=50; exit 1; }
echo "Ready."

# 4. k6 (native if present).
export K6_NO_USAGE_REPORT=true
SCRIPT=$([ "$SMOKE" = 1 ] && echo smoke.js || echo test.js)
if command -v k6 >/dev/null 2>&1; then
  ( cd "$RINHA" && k6 run "test/$SCRIPT" )
else
  docker run --rm --network host -e K6_NO_USAGE_REPORT=true \
    -v "$TESTDIR:/test" grafana/k6 run "/test/$SCRIPT"
fi

# 5. Report.
if [ -f "$TESTDIR/results.json" ]; then
  echo "===== RESULTS ====="; cat "$TESTDIR/results.json"
else
  echo "no results.json produced"
fi
