#!/usr/bin/env bash
# run-loadtest.sh — self-contained: bring up the stack and run the official load
# test (copied into ./loadtest) ENTIRELY inside the compose network, then print
# loadtest/results.json + mlock/OOM health. No sibling rinha repo required.
#
# Why in-network: the k6 script targets the LB. Reaching a published host port
# from a host-side / `--network host` k6 is flaky on Docker Desktop and fails
# outright if the stack isn't ready — a run of "connection refused", and since
# 0 requests succeed k6 has no http timing metric, so handleSummary throws and
# NO results.json is written. Attaching k6 to the compose network and pointing
# it at http://lb:9999 (TARGET env) is pure container-to-container networking:
# identical on Linux / macOS / Windows Docker Desktop, no host-port dependency.
# loadtest/compose.override.yml drops the LB's host port so the test never
# clashes with another stack already bound to 9999.
#
# Usage:
#   ./run-loadtest.sh           # full 120s load test, leave stack up
#   ./run-loadtest.sh --smoke   # quick smoke (5 requests)
#   ./run-loadtest.sh --down    # tear the stack down and exit
#
# Needs: docker + compose plugin. No native k6/curl (uses containers).
# The api image is amd64/AVX2 — run on an x86-64 host that has the avx2 flag.
# Branch under test:
#   submission -> docker-compose.yml uses PUBLIC v2 images (the exact submission)
#   main       -> docker-compose.yml builds the api from source; that build needs
#                 resources/references.json.gz (48MB, gitignored) — drop it in, or
#                 set RINHA=/path/to/rinha-de-backend-2026 and it is staged for you.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPOSE="${COMPOSE:-$ROOT/docker-compose.yml}"
OVERRIDE="$ROOT/loadtest/compose.override.yml"
RINHA="${RINHA:-$(cd "$ROOT/../rinha-de-backend-2026" 2>/dev/null && pwd || true)}"
DC=(docker compose -f "$COMPOSE" -f "$OVERRIDE")
SMOKE=0

for a in "${@:-}"; do case "$a" in
  --smoke) SMOKE=1 ;;
  --down)  "${DC[@]}" down; exit 0 ;;
  "" ) ;;
  *) echo "unknown arg: $a"; exit 2 ;;
esac; done

# ── preflight ──────────────────────────────────────────────────────────────
command -v docker >/dev/null || { echo "!! docker not found"; exit 1; }
[ -f "$ROOT/loadtest/test-data.json" ] || { echo "!! missing loadtest/test-data.json"; exit 1; }
grep -qo avx2 /proc/cpuinfo 2>/dev/null || echo "WARN: host has no avx2 flag — the amd64 image may not run natively here."

# ── if this compose builds the api, it needs the 48MB index source ──────────
if grep -q 'build:' "$COMPOSE"; then
  if [ ! -f "$ROOT/resources/references.json.gz" ]; then
    if [ -n "$RINHA" ] && [ -f "$RINHA/resources/references.json.gz" ]; then
      echo ">> staging references.json.gz from $RINHA"
      mkdir -p "$ROOT/resources"; cp "$RINHA/resources/references.json.gz" "$ROOT/resources/references.json.gz"
    else
      echo "!! this branch builds the api and needs resources/references.json.gz"
      echo "   provide it, or test the 'submission' branch (public images, no build)."
      exit 1
    fi
  fi
fi

# ── up ──────────────────────────────────────────────────────────────────────
echo ">> docker compose up -d --build"
"${DC[@]}" up -d --build

# ── resolve the compose network from the lb container ───────────────────────
LB_CID="$("${DC[@]}" ps -q lb || true)"
[ -n "$LB_CID" ] || { echo "!! lb container not created"; "${DC[@]}" ps; exit 1; }
NET="$(docker inspect -f '{{range $k,$_ := .NetworkSettings.Networks}}{{$k}}{{"\n"}}{{end}}' "$LB_CID" | head -1)"
echo ">> compose network: $NET"

# ── wait for /ready FROM INSIDE the network (same path k6 will use) ─────────
echo ">> waiting for http://lb:9999/ready ..."
ok=0
for _ in $(seq 1 45); do
  if docker run --rm --network "$NET" curlimages/curl:latest \
       -fsS --max-time 3 http://lb:9999/ready >/dev/null 2>&1; then ok=1; break; fi
  sleep 2
done
[ "$ok" = 1 ] || { echo "!! LB never became ready"; "${DC[@]}" logs --tail=80; exit 1; }
echo ">> ready."

# ── run k6 inside the network; workdir=/work so handleSummary writes loadtest/results.json
SRC="test.js"; [ "$SMOKE" = 1 ] && SRC="smoke.js"
echo ">> running k6 (in-network -> http://lb:9999, script loadtest/$SRC) ..."
rm -f "$ROOT/loadtest/results.json"
# --user 0:0: the k6 image runs as uid 12345 by default, but the bind-mounted
# /work is host-owned (root:root 755) on native Linux, so the non-root k6 user
# cannot write results.json ("permission denied"). On Docker Desktop the mount is
# world-writable so it slipped by; run k6 as root to write the summary everywhere.
# `|| true`: a handleSummary write failure makes k6 exit non-zero, which under
# `set -e` would abort before the HEALTH report — keep going so we always diagnose.
docker run --rm --network "$NET" --user 0:0 \
  -e K6_NO_USAGE_REPORT=true -e TARGET=http://lb:9999 \
  -v "$ROOT:/work" -w /work grafana/k6:latest run "loadtest/$SRC" || true

# ── report ──────────────────────────────────────────────────────────────────
echo; echo "===== RESULTS ====="
if [ -f "$ROOT/loadtest/results.json" ]; then cat "$ROOT/loadtest/results.json"; else echo "(smoke run, or no results.json produced)"; fi

echo; echo "===== HEALTH (mlock / OOM / faults) ====="
for svc in api1 api2; do
  cid="$("${DC[@]}" ps -q "$svc" || true)"
  [ -n "$cid" ] || { echo "$svc: NOT RUNNING"; continue; }
  vmlck="$(docker exec "$cid" grep VmLck /proc/1/status 2>/dev/null | tr -d '\n' || echo 'VmLck: ?')"
  # field 12 of /proc/1/stat = major faults since start. High = index getting
  # evicted and re-read from disk under load (the p99-collapse signature).
  majflt="$(docker exec "$cid" awk '{print $12}' /proc/1/stat 2>/dev/null || echo '?')"
  oom="$(docker inspect -f '{{.State.OOMKilled}}' "$cid")"
  mlk="$(docker logs "$cid" 2>&1 | grep -i mlock | tail -1 || true)"
  echo "$svc: ${vmlck:-VmLck: ?} | majflt=$majflt | OOMKilled=$oom | ${mlk:-NO mlock log}"
done
echo; echo "-- nproc seen by api1 (CPU available) / host --"
a1="$("${DC[@]}" ps -q api1 || true)"
[ -n "$a1" ] && docker exec "$a1" nproc 2>/dev/null || true
echo "host nproc: $(nproc 2>/dev/null || echo '?')  |  load: $(cat /proc/loadavg 2>/dev/null || echo '?')"

if [ ! -f "$ROOT/loadtest/results.json" ] && [ "$SMOKE" != 1 ]; then
  echo; echo "===== api1 startup log (mlock line + any WARN) ====="
  [ -n "$a1" ] && docker logs "$a1" 2>&1 | head -20 || true
fi

echo; echo "(stack left up; tear down with: $0 --down)"
