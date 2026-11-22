# Shopping Sessions v14 — exact native pipeline

C++20 candidate for [Shopping Sessions](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions). No runtime network, Python, external libraries, bundled data, or cached answers. IDs are opaque exact bytes.

## Submit

Use repository `saaranshgarg1/shopping-sessions`, branch `main`, with the commit reported after push. Dockerfile is at repository root. The evaluator launches the native executable directly.

```sh
docker build --platform linux/amd64 -t shopping-sessions .
docker run --rm --network none --read-only --user 1000:1000 \
  --cpus 8 --cpuset-cpus 0-7 --memory 24g --memory-swap 24g \
  -v /absolute/input:/data:ro -v /absolute/output:/output:rw \
  -v /absolute/scratch:/scratch:rw shopping-sessions
```

Output/scratch must be writable by uid 1000. Output table directories must be absent or empty. No arguments uses `DATA_DIR=/data`, `OUTPUT_DIR=/output`, `NCPU=8`; optional arguments are `shopping-sessions DATA_DIR OUTPUT_DIR`.

## Build and test locally

```sh
make all test
NCPU=8 build/shopping-sessions INPUT EMPTY_OUTPUT
make parquet-test PYTHON=/path/to/isolated/pyarrow/python
/path/to/isolated/python -I tests/verify.py fixtures EXTERNAL_FIXTURES build/shopping-sessions
/path/to/isolated/python -I tests/verify.py compare OUTPUT MATCHING_DEV_ANSWER
/path/to/isolated/python -I tests/differential.py build/shopping-sessions ORACLE_BINARY --cases 20
```

Polars/PyArrow are test-only. `BUILD_DIR` defaults to `build`; `NCPU` is capped by allowed CPU affinity. `SS_BASELINE=1` selects the preserved full-width reference; `SS_LEGACY=1` selects the former sprint engine. Windows wider than signed 32-bit milliseconds, or CPUs without AVX2, fall back to the reference.

## Engine

- Restored v5 shard-owned identity and exact event-selection path after v6 scored slower (3.3s versus 2.9522s). No fused per-row identity lookup or CRC experiment in this release.
- Alias descriptors carry exact packed short-ID keys or tagged fingerprints. Shard-local compact tables avoid rereading/re-hashing short names for every event. Arbitrary/long IDs always compare full bytes on fingerprint matches; no generator ID inversion.
- Repeat filter removed after v8 regression. Event selection now uses lossless short-ID keys in compact key/winner slots; tagged long/arbitrary IDs retain full-byte comparison. Empty-suffix and leading-zero IDs stay distinct.
- Input descriptor, purchase and identity-output buffers are preallocated; owner routing counts exact partition sizes before appending to avoid repeated reallocations.
- User Parquet columns borrow live ID/category string views, removing per-row string allocations and copies before page serialization.
- ISA-specialized whole-chunk scanner separates quote masks from comma/LF masks, removing per-vector indirect calls and transition overhead.
- Successful production execution is silent. Diagnostic stage timing is compile-time opt-in (`-DSS_PROFILE=1`), absent from submitted image.
- Persistent worker pool; LF-aligned mapped CSV chunks. Runtime AVX2/AVX-512F/BW dispatch, bounded loads at mapping ends, exact whole-field quote validation. CR and spaces remain data.
- Restored v12 timestamp and selection layout after v13 regression (2.1702s versus v12 2.0363s).
- Identity descriptors prefetch 16 future table positions with cached mixed keys; repeated-key hits no longer trigger table growth checks/rehashes.
- Category aggregation visits only purchase-bearing event indices collected during bin folding, avoiding a second full event scan.
- Checked F1/F2/F3 timestamps and purchase amounts. Valid ingest F1 copies compare chronologically by bytes. Calendar caching and date-based lateness shortcut preserve 24-hour boundaries.
- Producer-local 28-byte event rows plus 16-byte hash/chunk/row descriptors. One worker owns each exact event-ID shard. Full length/bytes resolve hash collisions; an invalid earliest copy suppresses later valid copies across users.
- Exact shard-owned alias dictionaries, union-find, and bytewise minimum person names including link-only IDs. Compact surviving rows route to canonical person owners; purchases use sparse payloads.
- v11 restores the winning v9 reducer and removes v10 early-bin folding after its 2.2962s regression against v9's 2.2446s.
- Identity shards emit compact surviving events and click references during interning instead of rewriting every source row's user handles. After union-find, each canonical root receives an owner partition and dense partition-local index once; routing carries that index directly into grouping, eliminating per-row grouping hash lookups.
- Half-hour event bins keep actual min/max/count/revenue. Occupied bins merge only when endpoint gap is at most 30 minutes; no per-event sort. Daily category totals use session-start date.
- Unsorted click intervals over session indices. Latest timestamp then bytewise smallest click ID wins credit; campaign bitsets count assists once. Identical click copies are idempotent, eliminating global click dedup and sorting.
- Per-worker campaign counters and independent person-owned Parquet files. DATE, real null definition levels, UTF-8 and int64 revenue. Checked footer close before exit, including empty tables.
- Release binary uses `-O3`, LTO, static C++ runtime linkage and stripping. AVX-512 is isolated behind CPU feature checks, not a global ISA requirement.

`SS_SIMD=avx2|avx512|auto` permits A/B measurements. No generator ID inversion, fixed manifest dates, approximation, or probabilistic dedup.

## Validation and performance

See [BENCHMARKS.md](BENCHMARKS.md). v11 passed all36 official fixtures, four differential datasets, native unit tests, matching fresh v5 dev tables in a silent offline read-only container, and complete20M-event/5M-click reference comparison including DATE, unique keys and reconciled totals. One laptop proxy run took2.62s; official performance remains unverified.

Current target is a correct official hidden-practice median **below 0.400s**, relaxed from 0.300s. Small dev runs do not satisfy that target. Submit, read the official result, then iterate on measured bottlenecks. Local median for 20M practice proxy was 3.79s, not below target.

`tests/prepare.py` downloads matching v5 artifacts into isolated directories and builds the generator with Rust 1.85+. `tests/bench.py` alternates fresh candidate/reference processes, hashes inputs/binaries/artifacts, records wall time/faults, and exact-compares typed tables. Native affinity is not CPU quota or no-swap enforcement; advisory eviction is not proof of cold cache. `tests/calibrate.cpp` exposes empty launch, structural scan, and output replay probes.
