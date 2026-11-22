# v5 verification and measured performance

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

Fresh workspace: `/scratch/user_data/saide/2024aib1015/shopping-v5-20261122.LPJkZS`. Slurm job **39970** requests eight CPUs and 24G, runs AVX2 versus AVX-512 correctness/timing on fresh v5 dev, practice and final sets. CPU feature support confirmed remotely. Existing HPC code/data remain untouched. Results pending when submission was prepared; HPC results are not official scorer results.

## Next acceptance gate

Submit current `main` commit on the web interface. Inspect correctness first, then official practice median. Use its stage/performance feedback to drive next revision. Five submissions per hour permits iteration, but neither passing tests nor newer AVX-512 hardware proves the target score in advance.
