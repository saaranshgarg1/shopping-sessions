#!/usr/bin/env python3
"""Compare fresh candidate/oracle processes on deterministic boundary-heavy CSVs."""
import argparse
from collections import Counter
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import random
import subprocess
import tempfile

import pyarrow as pa
import pyarrow.parquet as pq


def read(folder):
    tables = {}
    keys = {"user_daily": ("user_id", "day"), "campaign_daily": ("campaign_id", "day")}
    for name, columns in keys.items():
        paths = sorted((folder / name).glob("*.parquet"))
        assert paths, f"missing {name} Parquet"
        table = pa.concat_tables([pq.read_table(p) for p in paths])
        assert table.schema.field("day").type == pa.date32()
        assert all(table.schema.field(c).type == pa.int64() for c in table.column_names
                   if c not in ("user_id", "campaign_id", "channel", "day", "top_category"))
        rows = table.to_pylist()
        assert len({tuple(r[c] for c in columns) for r in rows}) == len(rows), f"duplicate {name} keys"
        tables[name] = Counter(tuple(r[c] for c in sorted(r)) for r in rows)
    users = [r for p in (folder / "user_daily").glob("*.parquet") for r in pq.read_table(p).to_pylist()]
    campaigns = [r for p in (folder / "campaign_daily").glob("*.parquet") for r in pq.read_table(p).to_pylist()]
    for column in ("sessions", "revenue_cents"):
        assert sum(r[column] for r in users) == sum(r[column] for r in campaigns), column
    assert sum(r["assisted_sessions"] for r in campaigns) >= sum(r["sessions"] for r in campaigns if r["campaign_id"] != "organic")
    return tables


def compare(left, right):
    a, b = read(left), read(right)
    for name in a:
        assert a[name] == b[name], (name, list((a[name] - b[name]).items())[:3], list((b[name] - a[name]).items())[:3])


def generate(folder, seed):
    rng = random.Random(seed)
    start = 1_835_395_200_000  # 2028-02-29, leap-day boundary
    end = start + 14 * 86_400_000
    stamp = lambda ms: datetime.fromtimestamp(ms / 1000, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    users = ["u", "u0", "u00", " U", "U ", "é", "u" + "0" * 40, "u" + "0" * 39 + "1"]
    users += [f"u{i}_{rng.getrandbits(128):032x}" for i in range(60)]
    events, clicks, links = [], [], []
    for i in range(40):
        links.append([users[i], users[i + 1]])
    links += [[users[0], "A-link-only"], [users[2], users[1]], [users[1], users[1]]]
    for i in range(1500):
        time = start + rng.randrange(14 * 86_400_000)
        if i < 300:
            time = start + (i // 3) * (1_800_000 + 1) + (i % 3)
        user = users[rng.randrange(len(users))]
        price = str(rng.randrange(1, 10000))
        kind = "purchase" if i % 5 == 0 else "view"
        ts = str(time).zfill(rng.choice((13, 20, 25, 70))) if i % 3 else stamp(time)
        row = [f"e{i}_{rng.getrandbits(128):032x}", user, ts, stamp(time), kind, f"p{i % 7}", price]
        if i % 19 == 0:
            later = row.copy(); later[1] = users[(i + 17) % len(users)]; later[3] = stamp(time + 10000)
            events.append(later)
            row[2] = "bad"
        elif i % 23 == 0:
            row[3] = stamp(time + 86_400_000 + 1000)
        elif i % 29 == 0:
            row[6] = "250\r"
        events.append(row)
        if i % 17 == 0:
            events.append(row.copy())
        if i % 3 == 0:
            for delta in (0, 1, -604_800_000, -604_800_001):
                clicks.append([f"k{i}_{delta}", user, f"c_{i % 4}", str(time + delta)])
            clicks += [[f"tie{i}A", user, "c_1", str(time)], [f"tie{i}a", user, "c_2", str(time)]]
    clicks += clicks[::17]
    events += [["", "u", str(start), stamp(start), "view", "p0", "0"],
               ["ignored_price", "unlinked", str(start), stamp(start), "view", "p0", "-not-digits"],
               ["null_category", "no-purchases", str(start), stamp(start), "cart", "p0", ""],
               ["midnight1", "midnight", str(start + 86_400_000 - 1000), stamp(start), "purchase", "p0", "100"],
               ["midnight2", "midnight", str(start + 86_400_000 + 1000), stamp(start), "purchase", "p1", "100"]]
    for name, rows, header in (("events", events, "event_id,user_id,ts,ingest_ts,event_type,product_id,price_cents"),
                               ("clicks", clicks, "click_id,user_id,campaign_id,ts"),
                               ("links", links, "user_id_a,user_id_b")):
        (folder / name).mkdir(parents=True)
        rng.shuffle(rows)
        parts = [[] for _ in range(5)]
        for row in rows:
            fields = [f'"{v}"' if rng.randrange(5) == 0 else v for v in row]
            parts[rng.randrange(4)].append(",".join(fields))
        parts[0] += ["malformed", 'bad"quote,not,whole,field', ""]
        for i, part in enumerate(parts):
            (folder / name / f"part-{i}.csv").write_text(header + "\n" + "\n".join(part) + ("\n" if i % 2 else ""), encoding="utf-8")
    (folder / "products.csv").write_text("product_id,category\np0,z\np1,é\np2,a\np3,a\np4,empty\n")
    (folder / "campaigns.csv").write_text("campaign_id,channel\nc_0,search\nc_1,email\nc_2,social\nc_3,search\n")
    (folder / "manifest.json").write_text(json.dumps({"window_start": stamp(start), "window_end": stamp(end)}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("oracle", type=Path)
    parser.add_argument("--data", type=Path)
    parser.add_argument("--cases", type=int, default=20)
    args = parser.parse_args()
    assert args.cases > 0
    with tempfile.TemporaryDirectory(prefix="shopping-diff-", dir=os.environ.get("SCRATCH_DIR")) as temporary:
        root = Path(temporary)
        for i in range(1 if args.data else args.cases):
            data = args.data.resolve() if args.data else root / f"input-{i}"
            if not args.data:
                generate(data, 39452 + i)
            outputs = []
            for label, binary in (("candidate", args.candidate), ("oracle", args.oracle)):
                output = root / f"{label}-{i}"
                env = {**os.environ, "SS_BASELINE": "1" if label == "oracle" else "0"}
                subprocess.run([str(binary.resolve()), str(data), str(output)], env=env, check=True,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=180)
                outputs.append(output)
            compare(*outputs)
            print(f"PASS differential {i}")


if __name__ == "__main__":
    main()
