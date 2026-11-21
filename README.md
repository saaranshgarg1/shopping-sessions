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
3. Scan separators, quotes and line boundaries together with portable SSE2. Pack eligible IDs losslessly into 64-bit keys; all other exact strings use tagged interned keys. Hash-partition all parsed rows by exact event ID, retaining invalid copies. Dedup with four-byte slots and stable block pointers before discarding invalid winners. Conflicting equal-ingest events or duplicate clicks restart the preserved generic engine before output writing; sorted file/chunk order determines its winner.
4. Validate ASCII digits, Gregorian calendar, timezone offset, windows, purchase amount, type, product ID and late-arrival boundary. Cached dates are accepted only after full calendar validation; arbitrary timestamp forms use the generic parser. Sparse or noncanonical catalog IDs use exact flat lookup instead of dense indexes. Hash-partition surviving rows by canonical person, not original user ID.
5. Count and scatter events/clicks directly into contiguous person blocks, then sort each person's full-width timestamps. People with at least 1,024 events use signed 64-bit radix sorting, skipping invariant bytes; smaller groups use comparison sorting. Equal timestamps share a session; split only at a gap greater than 30 minutes. Assign all session activity to the UTC start day. The preserved generic fallback retains its compact/radix path and full-width wide-window support.
6. Scan time-sorted clicks for exact last-click credit, with smallest byte-wise click ID winning ties. Assist counting uses per-campaign interval unions over sorted session indices. It never materializes a session-by-click or session-by-campaign join, including shared-device and whale people.
7. Reduce integer aggregates and write standard uncompressed PLAIN Parquet with native DATE, UTF-8 strings, nullable top category, and 64-bit metrics. Each user hash partition produces an independent file. Row order is irrelevant.

Default event processing uses 40-byte raw records, 24-byte surviving records and 16-byte person-block records on amd64. Dedup tables store 32-bit indices into stable winner-pointer arrays, not copied raw rows. Parquet pages write definition levels and values separately, avoiding a full-page copy. All decisions use exact input values; ID naming only selects a lossless representation, never semantics. No probabilistic dedup, fixed window, seed-specific code or approximate arithmetic. Set `SS_BASELINE=1` to select the preserved generic optimized engine.

## Verification

`make test` runs parser/calendar boundaries, 1,000 interval-union versus naive attribution cases, exact arbitrary-ID lookup checks, packed/generic key round trips and lexical ordering, 100,000 timestamp mutations per parser path, 30,000 SIMD-versus-generic CSV rows, copy-free dedup and deterministic fallback checks, radix multiset checks and compact/full-width aggregation equivalence. `make parquet-test PYTHON=/path/to/pyarrow-venv/bin/python` builds the native Parquet probe and checks schema, values, empty/all-null columns, definition-level padding and multiple pages with an independent decoder. PyArrow is a test-only dependency; the runtime remains dependency-free.

Official fixtures and public seed-1 answers must be supplied outside the repository. The bundled checker needs an isolated environment with `polars`:

```sh
/path/to/venv/bin/python -I tests/verify.py fixtures /external/fixtures /tmp/shopping-native-build/shopping-sessions
/path/to/venv/bin/python -I tests/verify.py compare /external/output /external/dev-answer
```

In this development environment, the fixture/data worker exposes `/tmp/shopping-buildathon-verify/READY` and its checker commands. The independent oracle and fuzz runner live under `/tmp/shopping-buildathon-audit/`.

See `BENCHMARKS.md` for measured checks, resource use, and limitations. Local timings do not predict leaderboard rank.
