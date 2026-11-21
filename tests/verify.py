#!/usr/bin/env python3
"""Exact external fixtures/answer check. Requires polars; run with Python -I."""
import csv
from collections import Counter
from datetime import date
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import polars as pl

SCHEMAS = {
    "user_daily": {"user_id": "string", "day": "date", "sessions": "integer", "events": "integer",
                   "revenue_cents": "integer", "top_category": "string"},
    "campaign_daily": {"campaign_id": "string", "channel": "string", "day": "date", "sessions": "integer",
                       "purchasing_sessions": "integer", "revenue_cents": "integer", "assisted_sessions": "integer"},
}


def compare(output, expected):
    for table, schema in SCHEMAS.items():
        folder = output / table
        if folder.is_symlink() or not folder.is_dir():
            raise ValueError(f"missing output directory: {folder}")
        paths = [p for p in folder.glob("*.parquet") if not p.name.startswith((".", "_"))
                 and not p.is_symlink() and p.is_file()]
        if not paths:
            raise ValueError(f"no Parquet in {folder}")
        actual = []
        for path in paths:
            frame = pl.read_parquet(path)
            if set(frame.columns) != set(schema):
                raise ValueError(f"wrong columns: {path}")
            for column, kind in schema.items():
                dtype = frame.schema[column]
                if kind == "date" and dtype != pl.Date:
                    raise ValueError(f"{column} must be DATE: {path}")
                if kind == "integer" and not dtype.is_integer():
                    raise ValueError(f"{column} must be integer: {path}")
                if kind == "string" and dtype.base_type() not in (pl.String, pl.Categorical, pl.Enum):
                    raise ValueError(f"{column} must be string: {path}")
            actual.extend(frame.select(list(schema)).rows())
        wanted = []
        with (expected / f"{table}.csv").open(encoding="utf-8", newline="") as handle:
            reader = csv.DictReader(handle)
            if set(reader.fieldnames or []) != set(schema):
                raise ValueError(f"wrong expected columns: {table}")
            for row in reader:
                wanted.append(tuple(date.fromisoformat(row[k]) if v == "date" else int(row[k]) if v == "integer"
                                    else None if k == "top_category" and row[k] == "" else row[k]
                                    for k, v in schema.items()))
        key_columns = [list(schema).index(k) for k in ("user_id", "campaign_id", "day") if k in schema]
        keys = [tuple(row[i] for i in key_columns) for row in actual]
        if len(set(keys)) != len(keys):
            raise ValueError(f"duplicate keys: {table}")
        if Counter(actual) != Counter(wanted):
            raise ValueError(f"{table}: missing {list((Counter(wanted)-Counter(actual)).elements())[:3]}, "
                             f"extra {list((Counter(actual)-Counter(wanted)).elements())[:3]}")
        print(f"MATCH {table}: {len(actual)} rows")


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ("fixtures", "compare"):
        sys.exit("usage: verify.py fixtures FIXTURES BINARY | compare OUTPUT EXPECTED_CSV_DIR")
    mode, left, right = sys.argv[1:]
    if mode == "compare":
        compare(Path(left), Path(right))
        return
    binary = str(Path(right).resolve())
    cases = [p for p in sorted(Path(left).iterdir()) if (p / "expected").is_dir()]
    if not cases:
        raise ValueError("no fixtures")
    for case in cases:
        data = case / ("input" if (case / "input").is_dir() else "data")
        with tempfile.TemporaryDirectory(prefix="shopping-check-", dir=os.environ.get("SCRATCH_DIR")) as output:
            subprocess.run([binary, str(data.resolve()), output], check=True)
            compare(Path(output), case / "expected")
        print(f"PASS {case.name}")
    print(f"{len(cases)}/{len(cases)} fixtures passed")


if __name__ == "__main__":
    main()
