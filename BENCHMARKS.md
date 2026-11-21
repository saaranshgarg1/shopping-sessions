# Verification and baseline measurements

Measured 2026-11-21. These are local development results, not a leaderboard score.

## Correctness gates

- Official unmodified checker: **36/36 fixtures passed**.
- Public generator seed 1, 1,000,000 requested events / 250,000 requested clicks: exact dev-answer match, **117,038 user_daily rows** and **27,400 campaign_daily rows**, no extra/missing rows or duplicate keys.
- Independent typed-model/oracle audit: **200/200 randomized datasets**, seed 314159. The harness additionally passed 10,809 strict CSV assertions and 3,033 timestamp assertions. This checks arbitrary UTF-8 identities, transitive links, malformed CSV, earliest-invalid dedup, time boundaries, session day assignment, ties and assists.
- AddressSanitizer + UndefinedBehaviorSanitizer: **50/50 further randomized datasets**, seed 271828, and all core assertions passed with no sanitizer findings.
- `make test`: **1,000 interval-union versus naive attribution cases**, plus strict parser/calendar assertions.
- `make parquet-test PYTHON=/path/to/pyarrow-venv/bin/python`: independent Arrow schema/value decoding for required/nullable columns, empty tables, null padding at 0–17 rows, all-null/no-mask pages, DATE/INT64 and multiple pages. Requires PyArrow only for testing, never at runtime.

External evidence in this development environment:

- `/tmp/shopping-buildathon-verify/results/primary-baseline-all.log`
- `/tmp/shopping-buildathon-verify/results/primary-fuzz-314159.log`
- `/tmp/shopping-buildathon-audit/primary-fuzz-314159/report.json`
- `/tmp/shopping-buildathon-verify/results/primary-unit-parquet.log`
- `/tmp/shopping-buildathon-verify/results/primary-baseline-bench.log`

No downloaded data, oracle code, environments or result artifacts are committed.

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

Existing HPC work owns representative 20M/5M and 60M/15M generation and benchmarking; seed 1 is public, not the hidden final seed. Large output invariants alone cannot establish hidden-answer equality. A sub-0.8-second 20M target has **not** been achieved or verified. No dashboard submission is performed by this integration session.
