# Verification and measured optimization

Measured 2026-11-21. These are development results, not a leaderboard score. Historical baseline checks below refer to release `9633a571f8633b19c186a160fcce472f0a3a2564`; optimized-source checks and comparisons are recorded separately.

## Baseline correctness gates

- Official unmodified checker: **36/36 fixtures passed**.
- Public generator seed 1, 1,000,000 requested events / 250,000 requested clicks: exact dev-answer match, **117,038 user_daily rows** and **27,400 campaign_daily rows**, no extra/missing rows or duplicate keys.
- Independent typed-model/oracle audit: **200/200 randomized datasets**, seed 314159. The harness additionally passed 10,809 strict CSV assertions and 3,033 timestamp assertions. This checks arbitrary UTF-8 identities, transitive links, malformed CSV, earliest-invalid dedup, time boundaries, session day assignment, ties and assists.
- Immutable committed binary: **100/100 further randomized datasets**, seed 161803 at NCPU=8, and **50/50**, seed 173205 at NCPU=128. Across these runs and the original audit, **400 distinct randomized datasets passed**.
- AddressSanitizer + UndefinedBehaviorSanitizer: **50/50 randomized datasets** of that total, seed 271828, and all core assertions passed with no sanitizer findings.
- `make test`: **1,000 interval-union versus naive attribution cases**, plus strict parser/calendar assertions.
- `make parquet-test PYTHON=/path/to/pyarrow-venv/bin/python`: independent Arrow schema/value decoding for required/nullable columns, empty tables, null padding at 0–17 rows, all-null/no-mask pages, DATE/INT64 and multiple pages. Requires PyArrow only for testing, never at runtime.

External evidence in this development environment:

- `/tmp/shopping-buildathon-verify/results/primary-baseline-all.log`
- `/tmp/shopping-buildathon-verify/results/primary-fuzz-314159.log`
- `/tmp/shopping-buildathon-audit/primary-fuzz-314159/report.json`
- `/tmp/shopping-buildathon-verify/results/primary-unit-parquet.log`
- `/tmp/shopping-buildathon-verify/results/primary-baseline-bench.log`

No downloaded data, oracle code, environments or result artifacts are committed.

## Optimized release correctness and local comparison

Integration independently rebuilt the fused-parse/compact-sort patch and passed **36/36 fixtures**, exact public seed-1 dev answers (**117,038 user_daily / 27,400 campaign_daily rows**, no missing/extra/duplicate keys), and **200/200 fresh randomized datasets**, seed 11212026 at NCPU=8. The independent typed-model harness also passed 10,809 strict CSV and 3,033 timestamp assertions. `make test` passed 1,000 attribution cases, 100,007 arbitrary-ID checks, 100,000 timestamp mutations, radix multiset checks and compact/full-width aggregation equivalence. PyArrow Parquet decoding and ASan+UBSan performance self-tests passed. An initial Parquet test selected an environment without PyArrow and failed with `ModuleNotFoundError`; rerunning with the existing decoder environment passed.

Same Intel i5-12450H, GCC 13.3.0, NCPU=8, flags and public 1M-event input; five alternating start-to-exit runs under the shared native-workload lock, warm client page cache, fresh outputs:

| Measurement | Verified baseline | Optimized |
| --- | --- | --- |
| Integration median wall seconds | 0.282051 | 0.224961 |
| Integration median peak RSS, KiB | 179804 | 205696 |
| Patch author's separate five-run median wall seconds | 0.283439 | 0.215045 |
| Patch author's median peak RSS, KiB | 179796 | 205896 |

Independent integration measured **20.24% less wall time (1.254x)**; the patch author's separate measurement was **24.13% less wall time (1.318x)**. Do not substitute the larger number for the independent result. Integration binary SHA256: `c40d37cec2119c83c24bac78d396706150dadb43589a94d08de5807b7c686074`. Evidence: `/tmp/shopping-integration-opt-evidence/local-benchmark.json`, `working-fixtures-dev.log`, `/tmp/shopping-integration-opt-fuzz/report.json`, and `/tmp/shopping-buildathon-perf-impl/bench-1795221385204995148/results.json`.

## Warm host baseline

Intel i5-12450H, 12 logical CPUs, Ubuntu Linux, GCC 13.3.0; C++20 `-O3 -DNDEBUG -pthread`, no `-march=native`. Public seed-1 dev occupies approximately 96.2 MB. Three start-to-exit runs per thread setting, each with fresh output directories. Local native workloads serialized with a shared file lock; the desktop and unrelated processes were not isolated. Input page cache was warm and not evicted.

| NCPU | Wall seconds, three runs | Median seconds | Peak RSS range, KiB |
| --- | --- | --- | --- |
| 1 | 1.16, 1.26, 1.18 | 1.18 | 169672–169852 |
| 2 | 0.61, 0.71, 0.67 | 0.67 | 170620–171016 |
| 4 | 0.36, 0.40, 0.44 | 0.40 | 173496–174464 |
| 8 | 0.32, 0.31, 0.32 | 0.32 | 179168–180312 |

The separate official fixture/dev validation used NCPU=2: 0.73 s start-to-exit, peak RSS 170640 KiB. Timings include parsing, exact dedup, sessionization, attribution and Parquet writing, not checking or compilation.

## Limits

Host runs do not enforce scorer CPU/RAM/no-swap isolation. Docker CLI is present but its daemon is absent. Rootless Podman is available; the existing HPC owner handles image builds and offline runtime checks to avoid duplicating large compiler images on the space-limited local disk. No container timing or cold-cache claim is made here.

## Representative HPC baseline

The existing HPC owner measured the same core source hashes on Intel Xeon Gold 6548Y+ with eight pinned physical cores, GCC 14.2.1, Lustre input, and RLIMIT_AS 24 GiB. These are not the scorer's eight hyperthreads or cgroup memory accounting. Source and per-run evidence: `/tmp/shopping-buildathon-verify/HPC/RESULTS.json` and `HPC/benchmark-39840/`.

| Public seed-1 input | Client-cold wall seconds | Warm wall seconds | Maximum RSS |
| --- | --- | --- | --- |
| 20M events / 5M clicks | 32.20, 21.31 | 16.42, 14.49 | 3.21 GiB |
| 60M events / 15M clicks | 64.69, 59.42 | 59.49, 44.22 | 9.06 GiB |

All eight runs exited successfully; repeated outputs matched exactly, with correct schema, unique keys and cross-table totals. Remote official 36 fixtures and 1M dev answers also matched. Client-cold means mincore measured zero resident input pages after per-file cache advice; Lustre server cache remains unknown. Wall times include the native pinning/environment wrapper, not Docker startup.

## Existing HPC job 39846: optimized comparison

Job 39846 completed successfully, eight runs, using the same hardware, CPU pinning, compiler flags and previously generated inputs as baseline job 39840. Its `src/main.cpp`, `src/core.hpp` and unchanged Parquet writer hashes match the integrated patch. Both 20M and 60M optimized outputs matched baseline outputs exactly; subsequent repetitions also matched, including schemas, unique keys and cross-table totals. Remote 36 fixtures and seed-1 dev answers passed.

| Input / version | Client-cold wall seconds | Warm wall seconds | Maximum RSS |
| --- | --- | --- | --- |
| 20M / baseline 39840 | 32.20, 21.31 | 16.42, 14.49 | 3.21 GiB |
| 20M / optimized 39846 | 27.29, 17.23 | 27.97, 14.43 | 3.64 GiB |
| 60M / baseline 39840 | 64.69, 59.42 | 59.49, 44.22 | 9.06 GiB |
| 60M / optimized 39846 | 71.44, 53.20 | 46.47, 46.90 | 11.05 GiB |

These are separate job blocks, **not alternating paired trials**. Optimized 20M warm median was **21.20 s versus 15.455 s (37.17% slower)**; optimized 60M warm median was **46.685 s versus 51.855 s (9.97% faster)**. Cold medians were 22.26 s versus 26.755 s for 20M and 62.32 s versus 62.055 s for 60M. Mixed results and substantial run variation do **not** establish a general large-input speedup. Peak RSS increased by approximately 0.43 GiB at 20M and 1.99 GiB at 60M; measured runs stayed below the 24 GiB address-space limit, not a verified scorer cgroup limit. Full evidence: `/tmp/shopping-buildathon-verify/HPC/benchmark-39846/` and baseline `HPC/RESULTS.json`.

Large exact baseline agreement does not establish hidden-answer equality. A sub-0.8-second 20M target has **not** been achieved or verified. No browser or further dashboard attempt was used by this integration session. The separate advanced engine was not modified or awaited for the historical 5c95735 integration.

## V3 independent release validation

V3 integrates the compact-key/person-block engine while preserving release `5c95735d02da562296a5b243cb74c3985a250a9f` as its parent, including flat identity/catalog lookup, the compact/radix fallback, prior tests, Dockerfile and configuration. `SS_BASELINE=1` selects that preserved optimized fallback, not historical baseline 9633a57. Conflicting equal-ingest or duplicate-click inputs restart fallback before output files are written. Fallback now preserves physical sorted-file/chunk order instead of worker completion order. This fixes an old-baseline nondeterminism observed in boundary case 14; historical releases are not rewritten.

Independent integrated-source gates: **36/36 fixtures**, exact public dev **117,038 user / 27,400 campaign rows**, **200/200 typed-model randomized cases** seed 211120265, **60/60 exact-ID boundary datasets** for both default v3 and preserved fallback, and **100/100 fallback randomized cases** seed 211120266. Core/performance/new fast-engine unit tests and independent PyArrow Parquet decoding passed; the new unit test also passed ASan+UBSan. Tests exercise lossless packed/generic ID round trips and ordering, 100,000 timestamp mutations, 30,000 SIMD/generic CSV rows, invalid earliest winners, conflicting ties, deterministic fallback, radix/full-width paths and writer errors.

Historical validation failures remain in external logs: the first integration oracle command supplied literal placeholders to an append-path CLI and exited with usage error; the corrected command passed all 200 cases. A comparative boundary run passed all v3 cases but the old baseline failed case 14; the corrected deterministic fallback passed all 60. Neither failure is reported as a successful run.

### Early verified export, job 39852

Native GCC 14.2.1, Intel Xeon Gold 6548Y+, cn15 initially idle, the same eight allocated physical cores 0–7 within NUMA node 0 for every candidate. All binaries use the same portable C++20 optimization/static-runtime flags, no native ISA tuning. Existing seed-1 20M/5M and 60M/15M inputs were copied into job-private tmpfs; every copied file matched its recorded SHA256. Inputs, outputs and scratch were node-local; staging, equality checking and post-exit fsync were outside candidate time. Each run prefetched inputs and verified mincore residency >99.9%; no systemwide eviction. Fresh outputs, serial order baseline/v3/v3/baseline/baseline/v3, three runs per candidate.

| Input | Baseline 9633a57 wall, s | Early v3 wall, s | Median speedup | V3 maximum RSS |
| --- | --- | --- | --- | --- |
| 20M / 5M clicks | 5.12, 4.96, 5.17 | 2.47, 2.08, 2.25 | 2.276x | 2.917 GiB |
| 60M / 15M clicks | 16.75, 14.37, 15.56 | 5.41, 6.61, 6.04 | 2.576x | 8.929 GiB |

Separate warm Lustre-input/tmpfs-output ABBA medians were 5.72→4.085 s (1.400x) at 20M and 20.17→10.93 s (1.845x) at 60M. These are device-separated comparisons, not pooled with tmpfs. All **22 full outputs** matched sorted typed baseline tables exactly, including schemas, unique keys and cross-table totals; 36 fixtures/dev also passed. Prior public optimized 5c95735 was run once per scale (3.53/10.99 s), a diagnostic rather than a median claim.

Evidence: `/tmp/shopping-buildathon-verify/HPC/release-v3/{results.json,integrated-build.log,integrated-validation.log,integrated-validation-corrected.log,boundary-fallback-validation.log,integrated-sanitizer.log,integration-audit.json}`; remote `/scratch/user_data/saide/2024aib1015/shopping-sessions/runs/release-v3-39852`.

### Integrated terminal source, job 39855

The final integrated source was compiled on cn15 and completed in 4m37s with **22/22 full output comparisons**, 36 fixtures and exact dev answers passing. The same GCC flags, cores 0–7, seed-1 inputs, SHA256 staging, prefetch/mincore gate, local output/scratch and serial schedule were used as above. All four source hashes matched the release working tree exactly. Previous optimized fallback and tests were retained, not replaced by the older baseline. The scan-floor/SIMD worker queued its next job with a dependency on this allocation, avoiding overlap.

| Input/device | Baseline wall, s | Integrated v3 wall, s | Baseline→v3 medians, s | Speedup | V3 peak RSS |
| --- | --- | --- | --- | --- | --- |
| 20M, tmpfs | 5.00, 5.01, 5.05 | 2.52, 2.53, 2.74 | 5.01→2.53 | 1.980x | 2.921 GiB |
| 60M, tmpfs | 15.42, 13.95, 14.53 | 7.03, 7.28, 6.58 | 14.53→7.03 | 2.067x | 8.674 GiB |
| 20M, warm Lustre input/local output | 6.61, 6.46 | 4.49, 4.27 | 6.535→4.38 | 1.492x | 2.917 GiB |
| 60M, warm Lustre input/local output | 20.38, 20.71 | 9.24, 10.14 | 20.545→9.69 | 2.120x | 8.670 GiB |

Tmpfs baseline peak RSS was 3.197/9.053 GiB at 20M/60M. Public optimized 5c95735 single-run diagnostics were 3.20/10.82 s and 3.610/10.961 GiB; one run cannot establish a median comparison. Final source is slower than the early export in these separate tmpfs suites; do not claim its layout changes improved speed using cross-job timings. It is nevertheless faster than baseline under matched final-job conditions at both scales, while reducing RSS and preserving deterministic fallback behavior. No >=5x milestone was observed.

Final evidence: `/tmp/shopping-buildathon-verify/HPC/release-v3/final-39855/` and remote `/scratch/user_data/saide/2024aib1015/shopping-sessions/runs/release-v3-final-39855`. Raw results retain user/system CPU time, percentage CPU, GNU elapsed, parent subprocess elapsed (separate), input residency, phase stderr, output bytes/blocks, faults/swaps, exact table comparisons and post-run fsync. Output equality checks compare every sorted row and typed value, not only hashes or aggregate totals.

### Selected final worker export, job 39859

The selected final export arrived before publication. Its only subsequent engine changes were an EOF pointer boundary fix and signed-64-bit person radix sorting at 1,024 events, with boundary/radix regressions. These were integrated without removing prior optimized fallback features. Independent integrated gates passed 36 fixtures, exact dev, core/perf/fast/PyArrow tests, 100 randomized cases seed 211120268 at NCPU=8, 100 cases seed 211120269 at NCPU=128, 124 exact-ID/forced-fallback datasets for both default and fallback, and ASan+UBSan fast tests.

Job 39859 completed 0:0 in 4m36s, cn15 cores 0–7, identical compile/input/cache/output conditions and serial schedule to job 39855. All **22 large outputs** matched complete sorted typed baseline tables; remote 36 fixtures/dev passed. Source hashes matched the integrated release exactly.

| Input/device | Baseline wall, s | Terminal v3 wall, s | Baseline→v3 medians, s | Speedup | V3 peak RSS |
| --- | --- | --- | --- | --- | --- |
| 20M, tmpfs | 5.01, 5.10, 4.96 | 2.50, 1.91, 2.49 | 5.01→2.49 | 2.012x | 2.936 GiB |
| 60M, tmpfs | 13.53, 16.59, 15.23 | 7.62, 6.60, 6.38 | 15.23→6.60 | 2.308x | 8.706 GiB |
| 20M, warm Lustre input/local output | 6.30, 6.39 | 2.03, 4.30 | 6.345→3.165 | 2.005x | 2.935 GiB |
| 60M, warm Lustre input/local output | 20.55, 20.66 | 11.07, 8.92 | 20.605→9.995 | 2.062x | 8.706 GiB |

The wide 20M Lustre v3 spread makes its two-run median uncertain; do not substitute its best run for a score. Tmpfs three-run medians support faster scale performance under these conditions, not a general filesystem or scorer claim. Single-run public 5c95735 diagnostics were 3.66/10.81 s. There is no >=5x result, no sub-0.8-second 20M result, and no robust extra radix end-to-end gain claim from comparing separate jobs.

Evidence: `/tmp/shopping-buildathon-verify/HPC/release-v3/terminal-39859/`, `terminal-validation.log`, `terminal-extra-validation.log`; remote `/scratch/user_data/saide/2024aib1015/shopping-sessions/runs/release-v3-terminal-39859`. Historical measurements above remain unchanged. The independent dedup-prefilter worker's patch and 20M-only results remain a separate follow-up, not silently merged or relabeled; its 60M validation was not available for this release. The SIMD experiment was not promoted because its measured end-to-end improvement was unreliable.

Native CPU results are **not scorer predictions**: physical Xeon cores are not eight N2 hyperthreads; RLIMIT_AS=24 GiB is not verified cgroup RSS/page-cache/no-swap enforcement (Slurm metadata reports AllocMem=0); process affinity restricts all threads to the same core set but does not individually bind each worker. Node was not reserved exclusively. Warm tmpfs is not cold storage or persistent output; Lustre server cache is unknown. GNU time has 10 ms precision and includes timeout/prlimit/startup; executable phase clocks exclude startup and checking. No >=5x/7x/8x/10x speedup or sub-0.8-second 20M result was observed.
