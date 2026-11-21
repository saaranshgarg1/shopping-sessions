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

Large exact baseline agreement does not establish hidden-answer equality. A sub-0.8-second 20M target has **not** been achieved or verified. No browser or further dashboard attempt was used by this integration session. The separate advanced engine was not modified or awaited.
