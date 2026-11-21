# Semantic hot-path eliminations

## Clicks: direct person routing

Official duplicate clicks have identical parsed ID, user, campaign and timestamp; CSV quotes and file placement may differ. Validation, identity resolution and partition selection therefore agree for every copy. Valid clicks go directly to person partitions. No global click-ID partition, hash table, dedup pass or raw-click record remains in the fast engine.

Last touch selects maximum timestamp, then minimum byte ID. Repeating the same candidate cannot change that maximum/minimum. The existing timestamp-ascending, byte-ID-descending sort still puts the winning distinct click last. Assist is a union of eligible session indices per campaign; repeated intervals add nothing because `covered[campaign]` already spans them. Neither operation counts click occurrences. Keeping copies rather than folding adds no new equality scan; duplicates only cost person-local sorting and attribution visits.

`dedup_test.cpp` writes 7,200 quoted/unquoted copies across 12 files, resolves linked aliases, verifies direct routing, distinct-ID tie-breaking and exactly-once campaign assists. Conflicting duplicate clicks are outside the official contract; `SS_BASELINE=1` retains the generic global-dedup engine for arbitrary conflicting input.

Publication intentionally precedes builds/tests, as requested. Verification must use an immutable export of the public branch SHA, not the mutable workspace.
