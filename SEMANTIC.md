# Semantic hot-path eliminations

## Clicks: direct person routing

Official duplicate clicks have identical parsed ID, user, campaign and timestamp; CSV quotes and file placement may differ. Validation, identity resolution and partition selection therefore agree for every copy. Valid clicks go directly to person partitions. No global click-ID partition, hash table, dedup pass or raw-click record remains in the fast engine.

Last touch selects maximum timestamp, then minimum byte ID. Repeating the same candidate cannot change that maximum/minimum. The existing timestamp-ascending, byte-ID-descending sort still puts the winning distinct click last. Assist is a union of eligible session indices per campaign; repeated intervals add nothing because `covered[campaign]` already spans them. Neither operation counts click occurrences. Keeping copies rather than folding adds no new equality scan; duplicates only cost person-local sorting and attribution visits.

`dedup_test.cpp` writes 7,200 quoted/unquoted copies across 12 files, resolves linked aliases, verifies direct routing, distinct-ID tie-breaking and exactly-once campaign assists. Conflicting duplicate clicks are outside the official contract; `SS_BASELINE=1` retains the generic global-dedup engine for arbitrary conflicting input.

## Event ingest: no calendar work when unnecessary

`ingest_ts` is guaranteed valid fixed-width F1 (`YYYY-MM-DDThh:mm:ssZ`). Once event `ts` has passed full validation, equal UTC date prefixes imply both times lie inside the same 24-hour interval; positive lateness is strictly below 24 hours. This holds for singleton and duplicate-candidate rows. No ingest calendar conversion runs in that case. Other valid event timestamps retain exact parsing and the strict `>24h` rejection rule, including offsets, epoch milliseconds and date boundaries.

Potential duplicates retain pointers into the live immutable mappings. A 20-byte comparison orders guaranteed-valid F1 chronologically: most significant calendar/time components precede less significant components and punctuation positions agree. Earliest-ingest selection therefore remains exact even when event `ts` is invalid; invalid winning records still suppress later valid records. Equal-ingest conflicting events keep the existing generic fallback. Non-F1 ingest shapes also invoke that fallback. Malformed F1 ingest is outside the guarantee; no new full ingest validator is added to the hot path.

Self-checks cover leap-day/year boundaries, reverse arrival, exact 24h and 24h+1s, offset and epoch event timestamps, lexical versus chronological ordering, and an invalid earliest event winner.

Publication intentionally precedes builds/tests, as requested. Verification must use an immutable export of the public branch SHA, not the mutable workspace.
