#!/usr/bin/env python3
"""Small differential CSV checks against the trusted audit oracle.

Usage:
  /tmp/shopping-buildathon-audit/venv/bin/python -I /tmp/shopping-sprint-v3/tests/sprint_check.py \
    CANDIDATE BASELINE [--cases 60] [--seed 3571113]

Only a fresh unique /tmp directory is written. The main session owns the shared flock
around builds/runs. This script does not build, time, install, publish or edit another checkout.
"""
import argparse
import importlib.util
import json
import os
from datetime import datetime, timedelta, timezone
from pathlib import Path
import subprocess
import tempfile

AUDIT = Path("/tmp/shopping-buildathon-audit")
spec = importlib.util.spec_from_file_location("shopping_audit_helpers", AUDIT / "audit.py")
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)
reference, compare, read_parquet = helpers.reference, helpers.compare, helpers.read_parquet

START, END = 1_792_713_600_000, 1_793_318_400_000
HEADER = {"events": "event_id,user_id,ts,ingest_ts,event_type,product_id,price_cents",
          "clicks": "click_id,user_id,campaign_id,ts"}
PRODUCTS = [("p0", "shoes"), ("p1", "books"), ("p2", "Å"), ("p3", "books")]
CAMPAIGNS = [("c_0", "search"), ("c_1", "social"), ("c_2", "email"), ("c_3", "search")]
PREFIXES = ["p1", "P1", "p01", "p001", "p0001", "p1 ", " p1", "p2", "P2", "p02", "p002"]


def timestamp(sequence):
    """Same instant in epoch, Z or numeric-offset form; zero-padding may exceed 20 bytes."""
    millis = START + sequence % 3 * 1000
    if sequence % 6 < 3:
        value = str(millis).zfill(20 if sequence % 6 == 0 else 25)
    elif sequence % 3 == 0:
        local = datetime.fromtimestamp(millis / 1000, timezone.utc)
        value = local.strftime("%Y-%m-%dT%H:%M:%SZ")
    else:
        minutes = (-660, 330, 720, -1439, 0)[sequence % 5]
        local = datetime.fromtimestamp(millis / 1000, timezone.utc) + timedelta(minutes=minutes)
        sign = "+" if minutes >= 0 else "-"
        value = local.strftime("%Y-%m-%dT%H:%M:%S") + f"{sign}{abs(minutes) // 60:02}:{abs(minutes) % 60:02}"
    assert reference.timestamp(value) == millis, (sequence, value)
    return value


def token(value, index=0):
    if '"' in value or "," in value:
        raise ValueError("generated field must not contain CSV delimiters or quote bytes")
    return f'"{value}"' if index % 2 else value


def write_csv(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    text = HEADER[path.parent.name] + "\n"
    text += "\n".join(",".join(token(value, index + offset)
                              for offset, value in enumerate(row)) for index, row in enumerate(rows)) + "\n"
    path.write_text(text, encoding="utf-8", newline="\n")


def split_rows(rows, count=4):
    """Round-robin files make the oracle and candidate see the same physical row order."""
    groups = [[] for _ in range(min(count, len(rows)))]
    for index, row in enumerate(rows):
        groups[index % len(groups)].append(row)
    return groups


def users_and_ids():
    # Exactly 8 suffix bytes; u with no suffix; short prefixes and >8-byte lexical ordering.
    users = ["u", "u12345678", "u1234567x", "u12345678x", "u1234567x9", "uABCDEFGH", "uABCDEFGX",
             "uabcdefgh", "uabcdefgX", "uABCDEFGHI", "uabcdefghi", "uA", "ua", "u!", "u~", "u[", "u{"]
    # The latter four cover every nonzero ASCII byte across cases, one per case.
    return users


def build_rows(case):
    """Return 60 deterministic small rows, with each case changing only the ASCII byte orbit."""
    users = users_and_ids()
    events, clicks = [], []
    for index, user in enumerate(users):
        events.append([f"e{index}", user, timestamp(index), str(START), "view", PREFIXES[index % len(PREFIXES)], "0"])
    # Numeric suffixes are literal strings: no leading-zero alias may be conflated.
    aliases = ["u0", "u00", "u000", "u0000", "u01", "u001", "u0001", "u08", "u008", "u0008",
               "u1", "u01", "u001", "u0001", "u8", "u08", "u008", "u0008",
               "u9", "u09", "u009", "u0009", "u!0", "u!00", "u!000", "u!0000", "u!1", "u!01", "u!001"]
    for index, user in enumerate(aliases):
        events.append([f"a{index}", user, timestamp(index % 3), str(START), "view", PREFIXES[index % len(PREFIXES)], "0"])
    # Include an explicitly renamed longer ID; u itself is not an empty-suffix escape.
    events.extend([["rename0", "user-longer", timestamp(1), str(START), "purchase", "p1", "250"],
                   ["rename1", "user-longer", timestamp(2), str(START), "view", "p2", "0"],
                   ["rename2", "é-user", timestamp(0), str(START), "view", "p3", "0"]])
    # Earlier invalid event rows win over later valid-looking rows.
    for index in range(4):
        instant = START + index * 1000
        events.append([f"d{index}", users[index], timestamp(index % 3),
                       str(START + (1 if index in (1, 2) else 0)), "purchase", "p1", "500"])
    # A later event with a different event timestamp cannot replace an earlier invalid copy.
    events.extend([["d1", users[1], timestamp(0), str(START + 1000), "view", "p2", "0"],
                   ["d2", users[2], timestamp(1), str(START - 1000), "purchase", "p3", "100"],
                   ["d3", users[3], timestamp(2), str(START - 2000), "view", "p3", "0"]])
    # Conflicting copies must force deterministic baseline fallback before output opens.
    if case % 7 == 0:
        events.extend([["forced-conflict", "uA", timestamp(0), str(START), "view", "p1", "0"],
                       ["forced-conflict", "uB", timestamp(0), str(START), "view", "p1", "0"]])
    if case % 11 == 0:
        clicks.extend([["forced-click-conflict", "uA", "c_0", timestamp(0)],
                       ["forced-click-conflict", "uB", "c_1", timestamp(1)]])
    # Equal-time click tie: lexical minimum, with names crossing 8-byte boundaries.
    tie_ids = ["k1234567x", "k12345678", "k1234567X", "k1234567y", "k12345678x",
               "kABCDEFGX", "kABCDEFGH", "kabcdefgX", "kABCDEFGHI", "kabcdefghi"]
    for index, click_id in enumerate(tie_ids):
        clicks.append([click_id, "tie-user", CAMPAIGNS[index % len(CAMPAIGNS)][0], timestamp(0)])
    # Distinct click IDs and case/punctuation aliases must remain distinct; include safe fallback IDs.
    distinct = [("k1", "c_1"), ("k01", "c_2"), ("k001", "c_3"), ("k0001", "c_0"),
                ("kA", "c_2"), ("ka", "c_1"), ("k!", "c_3"), ("k~", "c_0"),
                ("renamed-longer-id", "c_1"), ("é", "c_2"), ("k\x01", "c_3"),
                ("k0", "c_2"), ("k00", "c_3"), ("k000", "c_0"), ("k0000", "c_1")]
    for index, (click_id, campaign) in enumerate(distinct):
        clicks.append([click_id, users[(index + 8) % len(users)], campaign, str(START + (index % 2) * 1000)])
    # ASCII bytes 1..127 cover nonzero suffixes, except bytes CSV cannot carry raw.
    byte = case % 125 + 1
    if byte >= 34:
        byte += 1
    if byte >= 44:
        byte += 1
    char = chr(byte)
    for prefix in ("e", "u", "k"):
        ident = prefix + char
        if prefix in ("e", "u"):
            events.append([ident, "u" if prefix == "e" else ident, timestamp(0),
                           str(START), "view", "p1", "0"])
        else:
            clicks.append([ident, "u", "c_0", timestamp(0)])
    return events, clicks


def write_dataset(data, case):
    data.mkdir(parents=True, exist_ok=False)
    for name in ("events", "clicks"):
        (data / name).mkdir()
    (data / "links").mkdir()
    (data / "links" / "part-00000.csv").write_text("user_id_a,user_id_b\n", encoding="utf-8")
    (data / "products.csv").write_text("product_id,category\n" + "\n".join(
        ",".join(token(v, i) for i, v in enumerate(row)) for row in PRODUCTS) + "\n", encoding="utf-8")
    (data / "campaigns.csv").write_text("campaign_id,channel\n" + "\n".join(
        ",".join(token(v, i) for i, v in enumerate(row)) for row in CAMPAIGNS) + "\n", encoding="utf-8")
    (data / "manifest.json").write_text(json.dumps({"window_start": timestamp(0), "window_end": "2026-11-07T00:00:00Z"}))
    events, clicks = build_rows(case)
    for name, rows in (("events", events), ("clicks", clicks)):
        for part, group in enumerate(split_rows(rows)):
            write_csv(data / name / f"part-{part:05}.csv", group)


def check_oracle(data):
    expected = reference.solve(data)
    compare(expected, expected)  # audit invariants, schema and cross-table totals
    names = {row["user_id"] for row in expected["user_daily"]}
    # Distinct numeric aliases must not collapse; one exact-byte check catches the common bug.
    for alias in ("u", "u0", "u00", "u000", "u0000", "u01", "u001", "u0001", "u08", "u008", "u0008"):
        if alias not in names:
            raise ValueError(f"oracle lost distinct alias {alias!r}")
    return expected


def run_engine(command, data, output, work):
    command = [value.format(data=data, output=output) for value in command]
    with (work / f"{output.name}-stdout.log").open("w") as stdout, (work / f"{output.name}-stderr.log").open("w") as stderr:
        process = subprocess.run(command, cwd=AUDIT,
                                 env={**os.environ, "SS_BASELINE": "1" if output.name == "baseline" else "0"},
                                 stdout=stdout, stderr=stderr, timeout=120)
    if process.returncode:
        raise ValueError(f"exit {process.returncode}; see {work / (output.name + '-stderr.log')}")
    return read_parquet(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("--cases", type=int, default=60)
    parser.add_argument("--seed", type=int, default=3571113, help="reserved for future generation")
    args = parser.parse_args()
    if args.cases < 1:
        parser.error("--cases must be positive")
    commands = {name: [str(path.resolve()), "{data}", "{output}"]
                for name, path in (("candidate", args.candidate), ("baseline", args.baseline))}
    for name, command in commands.items():
        if not Path(command[0]).is_file():
            raise ValueError(f"{name} executable not found: {command[0]}")
    work = Path(tempfile.mkdtemp(prefix="shopping-sprint-v3-check-", dir="/tmp"))
    failures = []
    for case in range(args.cases):
        data = work / f"case-{case:03d}" / "input"
        write_dataset(data, case)
        expected = check_oracle(data)
        for name, command in commands.items():
            output = work / f"case-{case:03d}" / name
            try:
                compare(expected, run_engine(command, data, output, work / f"case-{case:03d}"))
            except Exception as error:
                failures.append({"case": case, "engine": name, "error": str(error)})
                print(f"FAIL case={case} engine={name}: {error}")
                break
        else:
            print(f"PASS case={case:03d}")
    report = {"work": str(work), "cases": args.cases, "passed": args.cases - len({f['case'] for f in failures}),
              "failures": failures}
    (work / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())