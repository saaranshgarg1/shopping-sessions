# v11 dense-owner routing candidate

## Current iteration

Official user-reported scores: winning v9 **2.2446s**, v10 early-bin folding **2.2962s**. v10 regression rejected; v11 restores the v9 reducer and removes early folding. Target remains **below 0.400s**.

v11 changes identity-to-person routing: each identity shard emits compact surviving event records and click references during the existing interning pass rather than writing user handles back into every source event/click row. After link union-find, canonical roots receive owner partitions and dense partition-local indices once. Emitted records route directly with those indices, removing per-row hash lookups during grouping. Exact full-byte collision handling and silent production execution remain required.

Verification passed: all36 official fixtures, four boundary-heavy differential datasets, adapted dense-owner/click-only ownership tests, full native unit suite, silent offline read-only dev container against fresh official answers, complete20M typed oracle comparison and focused independent gate. One laptop proxy run took2.62s; official v11 score remains pending.

---

# Rejected v10 early whale-bin folding candidate

## Previous iteration

Official user-reported winning v9 score: **2.2446s** (18% faster than v7). Target remains **below0.400s**.

v10 keeps v9 exact compact identity and folds non-purchase events for canonical people with >=8,192 surviving events into per-producer half-hour min/max/count summaries before person routing/grouping. Purchases remain exact sparse rows; reducer merges summary endpoints/counts with purchase bins before session/day/attribution. Counts include all aliases in linked/shared-device components and only valid dedup winners. No approximation or cached output; runtime silent.

Gate: all 36 fixtures, four differential datasets, 1,100 folded-versus-raw randomized histories, complete 20M proxy exact table comparison and silent offline dev container passed. One laptop 20M run took3.27s, not an official performance result. User-reported official v10 score: **2.2962s**, slower than winning v9 **2.2446s**.

---

# Winning v9 compact identity ownership candidate

## Previous iteration

Official user-reported scores: v7 **2.7389s**, v8 **2.8354s**. Rejected repeat filter and restored v7 event selection. Target remains **below 0.400s**.

v9 changes identity ownership only: 16-byte alias descriptors carry lossless short-ID keys or tagged fingerprints into shard-local compact key/handle tables. Repeated short aliases avoid mapped-name rereads and repeated hash computation. Long/arbitrary IDs still use full-byte comparisons on key matches. Component naming, links, sessions, attribution and output remain exact. Runtime remains silent.

Gate: 10,000 arbitrary-byte ID cases, forced fallback collisions, all 36 fixtures, four differential datasets and silent offline dev container passed. One laptop 20M run completed in 3.36s; not an official speed claim. User-reported official v9 score: **2.2446s**.

---

# Rejected v8 exact-repeat-filter candidate

## Previous iteration

Target relaxed to **below0.400s**. User-reported winning v7 score: **2.7389s**, 7.2% faster than v5 2.9522s. v8 keeps v7 scanner/identity/reducer and changes only event selection: a completed seen/repeat bitmap pass routes every member of a repeated bucket through exact dedup and skips table work for unflagged singleton buckets. Invalid earliest records remain in selection; full IDs resolve false repeats and hash collisions.

Quick gate: collision/singleton tests, 36 official fixtures and four differential datasets passed. Release container builds; official performance pending. This is not a claimed0.400s solution.

---

# Winning v7 scanner-only candidate

## Previous iteration

User-reported official scores: v5 **2.9522s**, v6 fused identity **3.3s**. Rejected v6 regression. v7 restores v5 identity, hash and exact event-selection pipeline while retaining silent runtime, Cascade Lake scheduling and ISA-specialized scanner loops. Scanner is the only execution-stage change relative to v5; no new fused lookup or repeat prefilter.

Quick gate passed all36 official fixtures, four boundary-heavy differential datasets and scanner/identity tests. Release image builds. Await official v7 score; no improvement or target claim yet.

---

# Rejected v6 candidate

## Prior iteration

Official user-reported v5 score: **2.9522s**. It misses 0.300s by 9.84x. v6 is a different candidate for scorer A/B, not a demonstrated target result.

v6 fuses linked identity resolution into input decoding, replaces the all-row alias ownership pass with worker-private singleton merging, uses CRC32 fingerprints plus exact byte equality, applies a completed repeat-bitmap prefilter before exact event selection, and specializes whole-chunk SIMD loops. Successful release runs emit no stdout/stderr; profile timing requires a separate `-DSS_PROFILE=1` build. Release instruction scheduling is tuned for Cascade Lake without globally requiring AVX-512.

Quick gate: all 36 fixtures, both matching v5 dev tables, four boundary-heavy differential datasets, exact ID/prefilter tests, scanner tests and full existing unit suite pass. Offline read-only release container matches public dev output and is silent. Independent focused review found no confirmed defects. One laptop 20M proxy run took 4.64s; this is not evidence of an improvement over v5. Scorer measurement is required before selecting this variant.

---

# Prior v5 verification and measured performance

Measured 2026-11-22. Official practice target below 0.300s is **not achieved or claimed**. This release replaces the former default execution path; preserves its reference as a fallback/oracle. Historical benchmark claims were removed to avoid confusing pre-v5 short-ID inputs with current v5 inputs.

## Matching artifacts

Fresh official downloads:

| Artifact | SHA-256 |
| --- | --- |
| generator.zip | `78f973f75d14b48e792628284f19c75de71fa597d20952e16a9ced93c78bee4a` |
| dev-answer.zip | `c2439e0e2e7456d721852814e9928e077648803d5c57a1760a40c7d3b0864d20` |

Public dev uses seed 1, 1,000,000 requested events / 250,000 clicks. Exact output: **118,107 user_daily** and **27,449 campaign_daily** rows. Older 117,038/27,400 answers are not the matching v5 set.

## Correctness gates

- Original native oracle independently passed all **36/36 official fixtures**.
- Default v5 engine and exact stripped LTO container binary both passed **36/36** fixtures.
- Both default output tables matched refreshed official dev answers exactly.
- **20 deterministic boundary-heavy CSV datasets** matched reference output: long/common-prefix UTF-8 IDs, cross-user invalid earliest duplicates, quotes/CR/empty fields, link chains and link-only minimum, ties, seven-day boundaries, midnight and nullable categories.
- **1,100 randomized reducer histories** plus explicit boundaries matched sort-based/exhaustive reference. Includes over-64-session histories, 672-session whales, arbitrary origin dates, credit ties, duplicate-click idempotence and reused scratch state.
- **60,000 generated CSV rows** checked against scalar parser; protected mapping-end pages checked bounded loads. Full 64-bit hash collisions, invalid earliest selection, pool recovery and linked naming checked explicitly.
- Existing core/performance/ID/calendar/radix tests pass. Independent PyArrow Parquet checks pass required/null/empty/all-null/multipage DATE and integer tables.
- Ingest tests and reducer tests passed AddressSanitizer plus UndefinedBehaviorSanitizer.
- Complete **20M-event / 5M-click** seed-49325 proxy outputs matched reference across paired fresh processes: typed DATE, unique keys, exact row values and reconciled session/revenue totals.

## Local performance, not official scoring

Host: Intel Core i5-12450H; eight pinned hardware threads (`0-7`); no AVX-512; resident tmpfs inputs. Native execution does not enforce 24GB/no-swap or CPU quota. Other host workloads and swap usage affected timing. Reported clock includes invocation, query, file close and process exit.

| Workload | v5 median | Full-width reference median |
| --- | ---: | ---: |
| Dev seed 1: 1M / 250k | 0.201281s | 0.235750s |
| Practice proxy seed 49325: 20M / 5M | 3.792494s | 6.786031s |

Practice input event/click/link bytes: **2,036,980,662**. Physical rows including dirt: **27,016,029**. Structurally valid nonempty-ID event descriptors: **20,916,260**, **334,660,160 descriptor bytes**. Exact duplicate comparisons: **924,311**. Largest shard: **164,539**. Distinct aliases: **1,485,635**. Purchase payloads: **585,389**.

One practice run diagnostic: map/catalog 15ms, scan/validate 887ms, exact selection 433ms, alias/union 1,577ms, route 152ms, bins/coverage/Parquet close 625ms. Alias lookup and decoding remain dominant; current implementation is not a 300ms practice solution.

Calibration: empty native invocation 2.2ms; actual-size resident structural scan 156ms; public-answer parsing plus Parquet replay 42.5ms. These are probes, not full-query timing.

A/B experiments with producer-local alias interning and extra alias radix buckets were slower on this host and removed. High-bit alias shard ownership avoids low hash bits fixed by partitioning from clustering the internal lookup table. Per-campaign direct coverage-slot lookup avoids whale click-by-campaign scans.

## Container verification

Final image successfully builds from Debian bookworm with GCC 12, `-O3 -flto -fno-semantic-interposition`, static libstdc++/libgcc and stripped binary. Image approximately **80.7MB**. Release image ID: `22a20c39ba5ee5bf6ecaf2351628fd167ecf1e105a8e6dbd6e5ebd172486a798`.

Exact executable SHA-256: `4cfae134704f0d8c49febb33f04b9db1f59cc22124fadb751554f2ded237493e`.

Container executed successfully with network disabled, read-only root and input, writable output/scratch, uid 1000 and inherited eight-CPU affinity. Both tables matched official dev answers. Local rootless runtime cannot apply its requested cpuset controller (`cpuset.cpus` missing), so **24GB/no-swap/CPU-quota isolation was not verified locally**. This runtime limitation is not a build or pipeline failure.

## HPC rehearsal

Fresh workspace: `/scratch/user_data/saide/2024aib1015/shopping-v5-20261122.LPJkZS`. Slurm job **39970** requests eight CPUs and 24G, runs AVX2 versus AVX-512 correctness/timing on fresh v5 dev, practice and final sets. CPU feature support confirmed remotely. Existing HPC code/data remain untouched. Dev/practice tables passed exact reference comparisons. Warm Lustre practice medians were AVX2 **19.817s** and AVX-512 **18.633s**; final rehearsal remains running. These shared-filesystem numbers are slower than laptop tmpfs measurements and are not official scorer results.

## Next acceptance gate

Submit current `main` commit on the web interface. Inspect correctness first, then official practice median. Use its stage/performance feedback to drive next revision. Five submissions per hour permits iteration, but neither passing tests nor newer AVX-512 hardware proves the target score in advance.
