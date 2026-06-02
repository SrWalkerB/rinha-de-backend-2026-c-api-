# Rinha de Backend 2026 — Fraud Detection in C

Vector-kNN fraud detection. Custom epoll HTTP API + custom round-robin load
balancer, both in C. Targets **p99 ≤ 1 ms, 0 % failure → score 6000**.

## How it works

`POST /fraud-score` turns each payload into a 14-dim vector, finds the 5 nearest
neighbors (squared Euclidean) in a fixed 3,000,000-vector reference set, and
returns `fraud_score = frauds/5`, `approved = score < 0.6`. `GET /ready` → 200.

Everything is a **bit-exact port of the reference labeler**
(`../rinha-de-backend-2026/data-generator/main.c`): same `round4`, same
normalization, same strict-`<` 5-NN with lower-index tie-break.

### Speed: bucketing + int8 filter → int16 exact re-rank
- At **build time**, `prepare` decompresses `references.json.gz`, quantizes each
  dim to **int16 = round(v*10000)** (lossless on the round4 grid) AND **int8 =
  round(v*127)** (coarse), and groups the 3M vectors into **16 buckets** keyed by
  `(is_online, card_present, unknown_merchant, has_last)`. Output: `packed.bin`
  (~149 MB: int8 + int16 + orig-index + fraud bitset), baked into the image.
- At **query time**, only the matching bucket is searched, in two passes:
  1. **int8 AVX2 sequential scan** of the whole bucket → shortlist of the 64
     nearest by approximate distance (half the memory traffic of int16 → fast,
     predictable, no high-dimensional kd-tree tail).
  2. **int16 exact** squared distance over just those 64 → exact 5-NN, with the
     `(distance, original-index)` tie-break that reproduces `main.c` bit-for-bit.
  The int8 shortlist always contains the true 5-NN; `verify` proves 0 mismatch.
- Buckets are exact because a boolean/sentinel mismatch adds ≥1.0 to squared
  distance, far beyond the NN radius. `mcc_risk` is a searched dim (not bucketed).
- `packed.bin` is `mmap(MAP_SHARED, PROT_READ)` → shared page cache across both
  api instances (charged once).

### Topology (1 CPU / 350 MB cap)
```
:9999  lb (C, epoll, round-robin)  ──>  api1 :8001 (C, epoll)
                                   └──>  api2 :8001 (C, epoll)
```
| service | cpus | memory |
|---|---|---|
| lb   | 0.10 | 24 MB  |
| api1 | 0.45 | 150 MB |
| api2 | 0.45 | 150 MB |

The LB never inspects payloads (round-robin only) — compliant with the rules.

## Layout
```
src/common/  vec.c packed.c knn.c reqparse.c   detection core + parser
src/prepare/ prepare.c                          build-time index builder
src/api/     api.c                              epoll HTTP server
src/lb/      lb.c                               epoll round-robin proxy
src/verify/  verify.c                           offline parity gate
```

## Build & test locally

Requires Docker. From this directory:

```powershell
# full load test (stages dataset, builds, waits /ready, runs official k6)
.\run-local.ps1
# quick smoke
.\run-local.ps1 -Smoke
# tear down
.\run-local.ps1 -Down
```
```bash
./run-local.sh          # or --smoke / --down / --no-build
```
Reads the score from `../rinha-de-backend-2026/test/results.json`.

### Parity gate (run before trusting the engine)
```bash
make prepare verify
./prepare ../rinha-de-backend-2026/resources/references.json.gz packed.bin
./verify packed.bin ../rinha-de-backend-2026/test/test-data.json
```
Must report **0 approved mismatches** vs expected and **0 bucketed≠full**.

## Submission

1. Build + push public amd64 images:
   ```powershell
   .\build-images.ps1 -User <dockerhub-user> -Push
   ```
2. On the `submission` branch, ship `docker-compose.submission.yml` as
   `docker-compose.yml` (replace `<DOCKERHUB_USER>`), plus `info.json` + `LICENSE`.
3. `main` branch holds the source. Register via PR adding
   `participants/<github-user>.json` to the upstream repo.
4. Open an issue with `rinha/test` to run the official test.

## Notes / tuning
- p99's biggest risk is **CPU-quota throttling**; the hot path is alloc-free,
  TCP_NODELAY + keep-alive everywhere, no `sqrt`, one AVX2 load per ref.
- Build is AVX2 (`-mavx2`, Mac Mini Late 2014 = Haswell). A scalar fallback
  compiles automatically if `__AVX2__` is undefined.
