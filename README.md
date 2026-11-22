# Shopping Sessions v6 — exact native pipeline

Submission-ready C++20 executable for [Shopping Sessions](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions). No runtime network, Python, external libraries, bundled data, or cached answers. IDs are opaque exact bytes.

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

- Fused immutable linked-ID lookup and worker-private exact singleton interning during input decoding; no separate all-row alias scatter/intern pass. Hardware CRC32 fingerprints retain full-byte collision checks.
- Completed repeat-bit prefilter sends all repeated buckets to exact event selection; singleton hash buckets bypass table insertion without approximating dedup.
- ISA-specialized whole-chunk scanner separates quote masks from comma/LF masks, removing per-vector indirect calls and transition overhead.
- Successful production execution is silent. Diagnostic stage timing is compile-time opt-in (`-DSS_PROFILE=1`), absent from submitted image.
- Persistent worker pool; LF-aligned mapped CSV chunks. Runtime AVX2/AVX-512F/BW dispatch, bounded loads at mapping ends, exact whole-field quote validation. CR and spaces remain data.
- Checked F1/F2/F3 timestamps and purchase amounts. Valid ingest F1 copies compare chronologically by bytes. Calendar caching and date-based lateness shortcut preserve 24-hour boundaries.
- Producer-local 28-byte event rows plus 16-byte hash/chunk/row descriptors. One worker owns each exact event-ID shard. Full length/bytes resolve hash collisions; an invalid earliest copy suppresses later valid copies across users.
- Exact shard-owned alias dictionaries, union-find, and bytewise minimum person names including link-only IDs. Compact surviving rows route to canonical person owners; purchases use sparse payloads.
- Half-hour event bins keep actual min/max/count/revenue. Occupied bins merge only when endpoint gap is at most 30 minutes; no per-event sort. Daily category totals use session-start date.
- Unsorted click intervals over session indices. Latest timestamp then bytewise smallest click ID wins credit; campaign bitsets count assists once. Identical click copies are idempotent, eliminating global click dedup and sorting.
- Per-worker campaign counters and independent person-owned Parquet files. DATE, real null definition levels, UTF-8 and int64 revenue. Checked footer close before exit, including empty tables.
- Release binary uses `-O3`, LTO, static C++ runtime linkage and stripping. AVX-512 is isolated behind CPU feature checks, not a global ISA requirement.

`SS_SIMD=avx2|avx512|auto` permits A/B measurements. No generator ID inversion, fixed manifest dates, approximation, or probabilistic dedup.

## Validation and performance

See [BENCHMARKS.md](BENCHMARKS.md). All 36 official fixtures and both matching fresh v5 public dev tables passed. Complete 20M-event/5M-click proxy tables matched the reference, including DATE, unique keys and reconciled totals. Parser/reducer randomized checks and AddressSanitizer/UndefinedBehaviorSanitizer passed.

Target remains a correct official hidden-practice median **below 0.300s**. Laptop dev runs below 300ms do not satisfy that target. Submit, read the official result, then iterate on measured bottlenecks. Local median for 20M practice proxy was 3.79s, not below target.

`tests/prepare.py` downloads matching v5 artifacts into isolated directories and builds the generator with Rust 1.85+. `tests/bench.py` alternates fresh candidate/reference processes, hashes inputs/binaries/artifacts, records wall time/faults, and exact-compares typed tables. Native affinity is not CPU quota or no-swap enforcement; advisory eviction is not proof of cold cache. `tests/calibrate.cpp` exposes empty launch, structural scan, and output replay probes.
