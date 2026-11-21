# Shopping Sessions — native exact pipeline

Standalone C++20 implementation of the [Shopping Sessions track](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions). No runtime dependencies beyond Linux and libc. No data, binaries, or downloaded archives belong in this repository.

## Build and run

```sh
make
make test
NCPU=8 /tmp/shopping-native-build/shopping-sessions /path/to/data /path/to/empty-output
```

With no arguments, reads `DATA_DIR` (default `/data`) and writes `OUTPUT_DIR` (default `/output`). `NCPU` defaults to 8; lower it on small hosts. Build artifacts default to `/tmp/shopping-native-build`; override `BUILD_DIR` when needed. `SCRATCH_DIR` and `MEM_GB` are supplied by the evaluator but are not required: this implementation does not spill to disk. Output table folders must be absent or empty; existing output is never silently overwritten.

```sh
docker build --platform linux/amd64 -t shopping-sessions .
docker run --rm --platform linux/amd64 --network none --read-only \
  --user 1000:1000 --cpus 8 --memory 24g --memory-swap 24g \
  --tmpfs /tmp:rw,size=64m \
  -v /absolute/data:/data:ro -v /absolute/output:/output \
  -v /absolute/scratch:/scratch shopping-sessions
```

Mounted output and scratch folders must be writable by uid 1000. Docker downloads build images only; the executable requires no network. Compiler and runtime use the same Debian release; C++ runtime libraries are statically linked.

## Exact semantics and architecture

1. Memory-map every input CSV file. Parse only LF as a line ending; strip whole-field quotes, never trim or normalize values. Work is divided at row boundaries into 4 MiB chunks for balanced parallel ingest.
2. Resolve all valid links with path-compressed union-find, keeping the byte-wise smallest identity, including IDs with no activity. Read-only string views refer to mapped input for the entire run.
3. Scan event IDs into atomic seen/repeated bitmaps, then validate every parsed row. IDs whose hash bit never repeated are provably unique and go directly to canonical-person partitions; repeated bits retain all copies for exact earliest-ingest dedup. Bitmap collisions only add exact candidates. Invalid earliest copies still poison their ID. Click IDs receive exact global dedup.
4. Runtime-dispatch 64-byte delimiter scanning to AVX-512BW, AVX2, or SSE2 according to CPU and OS support. Preserve generic CSV parsing for irregular rows. Validate ASCII digits, Gregorian calendar, timezone offset, windows, purchase amount, type, product ID and late-arrival boundary. Cached dates require calendar validation; other timestamp forms use the generic parser. Short prefixed ASCII IDs use reversible packed keys; all other IDs retain exact string keys.
5. Group surviving events and clicks into compact per-person blocks, then sort each person's timestamps. Equal timestamps share a session; split only at a gap greater than 30 minutes. Assign all session activity to the UTC start day. Conflicting equal-ingest events or duplicate clicks restart the deterministic generic engine before output files are opened.
6. Scan time-sorted clicks for exact last-click credit, with smallest byte-wise click ID winning ties. Assist counting uses per-campaign interval unions over sorted session indices. It never materializes a session-by-click or session-by-campaign join, including shared-device and whale people.
7. Reduce integer aggregates and write standard uncompressed PLAIN Parquet with native DATE, UTF-8 strings, nullable top category, and 64-bit metrics. Each user hash partition produces an independent file. Row order is irrelevant.

The default engine uses 24-byte surviving event records and 16-byte per-person event records on amd64. Exact-candidate tables store 32-bit row indices. The bitmaps are conservative filters, not probabilistic dedup decisions; arbitrary ID naming and full-width event times remain supported. `SS_BASELINE=1` forces the generic engine, `SS_DEDUP=0` disables the event bitmap optimization, and `SS_SIMD=sse2|avx2|avx512` caps scanner selection, falling back safely when an ISA is unavailable. Defaults select the combined fast path and automatic ISA dispatch.

## Verification

`make test` runs parser/calendar boundaries, 1,000 interval-union versus naive attribution cases, exact arbitrary-ID lookup checks, 100,000 timestamp mutations, radix multiset checks and compact/full-width aggregation equivalence. It also checks bitmap races/collisions, invalid winners, deterministic fallback, every supported scanner with guard-page tails, randomized CSV equivalence, and cached timestamp decoding. `make parquet-test PYTHON=/path/to/pyarrow-venv/bin/python` builds the native Parquet probe and checks schema, values, empty/all-null columns, definition-level padding and multiple pages with an independent decoder. PyArrow is a test-only dependency; the runtime remains dependency-free.

Official fixtures and public seed-1 answers must be supplied outside the repository. The bundled checker needs an isolated environment with `polars`:

```sh
/path/to/venv/bin/python -I tests/verify.py fixtures /external/fixtures /tmp/shopping-native-build/shopping-sessions
/path/to/venv/bin/python -I tests/verify.py compare /external/output /external/dev-answer
```

In this development environment, the fixture/data worker exposes `/tmp/shopping-buildathon-verify/READY` and its checker commands. The independent oracle and fuzz runner live under `/tmp/shopping-buildathon-audit/`.

See `BENCHMARKS.md` for measured checks, resource use, and limitations. Local timings do not predict leaderboard rank.
