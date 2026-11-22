#!/usr/bin/env python3
"""Fetch matching official v5 artifacts into unique directories; build from safe cwd."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from urllib.request import urlopen
import zipfile

BASE = "https://www.joinbuilders.org/buildathon/tracks/shopping-sessions/files/"


def unpack(archive, root):
    with zipfile.ZipFile(archive) as z:
        for member in z.infolist():
            path = Path(member.filename)
            if path.is_absolute() or ".." in path.parts or ((member.external_attr >> 16) & 0o170000) == 0o120000:
                raise ValueError(f"unsafe archive member: {member.filename}")
        z.extractall(root)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--artifacts", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--events", type=int, default=1000000)
    p.add_argument("--clicks", type=int, default=250000)
    p.add_argument("--threads", type=int, default=8)
    args = p.parse_args()
    if args.output.exists():
        p.error("refusing to overwrite existing dataset")
    if args.events < 1000 or args.clicks < 0 or args.threads < 1:
        p.error("events >=1000, clicks >=0 and threads >=1 required")
    compiler = shutil.which("rustc")
    if not compiler:
        p.error("Rust 1.85+ compiler required; install it before preparing datasets")
    args.artifacts.mkdir(parents=True, exist_ok=True)
    bundle = Path(tempfile.mkdtemp(prefix="v5-", dir=args.artifacts)).resolve()
    files = {}
    for name in ("generator.zip", "dev-answer.zip", "starter-duckdb.zip", "rules.md"):
        target = bundle / name
        with urlopen(BASE + name, timeout=60) as response, target.open("wb") as f:
            shutil.copyfileobj(response, f)
        with target.open("rb") as f:
            files[name] = hashlib.file_digest(f, "sha256").hexdigest()
        if name.endswith(".zip"):
            extraction = bundle / (name.removesuffix(".zip") + "-extracted")
            extraction.mkdir()
            unpack(target, extraction)
    build = bundle / "build"
    build.mkdir()
    binary = build / "generate"
    source = bundle / "generator-extracted/generator/src/main.rs"
    # Build outside downloaded sources; dependencies are only Rust standard library.
    subprocess.run([compiler, "--edition=2024", "-C", "opt-level=3", "-C", "panic=abort", str(source), "-o", str(binary)],
                   cwd=build, check=True)
    command = [str(binary), "--seed", str(args.seed), "--events", str(args.events), "--clicks", str(args.clicks),
               "--threads", str(args.threads), "--output", str(args.output.resolve())]
    subprocess.run(command, cwd=build, check=True)
    (bundle / "artifacts.json").write_text(json.dumps({"sha256": files, "command": command,
        "compiler": subprocess.run([compiler, "--version"], capture_output=True, text=True, check=True).stdout.strip(),
        "matching_answer_args": {"seed": 1, "events": 1000000, "clicks": 250000}}, indent=2))
    print(bundle)


if __name__ == "__main__":
    main()
