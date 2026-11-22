#!/usr/bin/env python3
"""Start-to-exit paired timing with fresh outputs, hashes, faults and CPU affinity."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import shutil
import statistics
import subprocess
import tempfile
import time


def sha256(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("data", type=Path)
    p.add_argument("candidate", type=Path)
    p.add_argument("--oracle", type=Path)
    p.add_argument("--runs", type=int, default=3)
    p.add_argument("--cpus", default="0-7")
    p.add_argument("--cache", choices=("warm", "evict-request"), default="warm")
    p.add_argument("--output-root", type=Path, required=True)
    p.add_argument("--report", type=Path, required=True)
    p.add_argument("--generator", type=Path)
    p.add_argument("--generator-archive", type=Path)
    p.add_argument("--answers-archive", type=Path)
    p.add_argument("--seed", type=int)
    p.add_argument("--events", type=int)
    p.add_argument("--clicks", type=int)
    p.add_argument("--docker-image", help="Container timing instead of native process; oracle remains native")
    args = p.parse_args()
    if args.runs < 1:
        p.error("--runs must be positive")
    if args.report.exists():
        p.error("refusing to overwrite existing report")
    args.output_root.mkdir(parents=True, exist_ok=True)
    data = args.data.resolve()
    paths = sorted(data.rglob("*.csv")) + [data / "manifest.json"]
    inventory = {}
    for path in paths:
        count = 0
        with path.open("rb") as f:
            for block in iter(lambda: f.read(1 << 20), b""):
                count += block.count(b"\n")
        inventory[str(path.relative_to(data))] = {"bytes": path.stat().st_size, "physical_lines": count,
                                                   "sha256": sha256(path)}
    cpus = set()
    for part in args.cpus.split(","):
        lo, _, hi = part.partition("-")
        cpus.update(range(int(lo), int(hi or lo) + 1))
    if not cpus or not cpus <= os.sched_getaffinity(0):
        p.error("--cpus must be subset of allowed CPU affinity")
    versions = {"candidate": sha256(args.candidate), **({"oracle": sha256(args.oracle)} if args.oracle else {})}
    records = []
    try:
        for iteration in range(args.runs):
            engines = [("candidate", args.candidate)] + ([("oracle", args.oracle)] if args.oracle else [])
            if iteration % 2:
                engines.reverse()
            for label, binary in engines:
                if args.cache == "warm":
                    for path in paths:
                        with path.open("rb") as f:
                            while f.read(1 << 20):
                                pass
                else:
                    # Advisory eviction is not proof of client-cold residency.
                    for path in paths:
                        with path.open("rb") as f:
                            os.posix_fadvise(f.fileno(), 0, 0, os.POSIX_FADV_DONTNEED)
                output = Path(tempfile.mkdtemp(prefix=f"{label}-{iteration}-", dir=args.output_root))
                env = {**os.environ, "NCPU": str(len(cpus)), "SS_BASELINE": "1" if label == "oracle" else "0"}
                command = ["taskset", "-c", args.cpus, str(binary.resolve()), str(data), str(output)]
                if args.docker_image and label == "candidate":
                    command = ["docker", "run", "--rm", "--network", "none", "--read-only",
                               "--cpus", str(len(cpus)), "--cpuset-cpus", args.cpus,
                               "--memory", "24g", "--memory-swap", "24g", "--user", f"{os.getuid()}:{os.getgid()}",
                               "-v", f"{data}:/data:ro", "-v", f"{output.resolve()}:/output:rw",
                               "--tmpfs", "/scratch:rw,nosuid,nodev,size=1g", args.docker_image]
                before = resource.getrusage(resource.RUSAGE_CHILDREN)
                start = time.perf_counter_ns()
                process = subprocess.run(command, env=env, capture_output=True, text=True, timeout=180)
                elapsed = (time.perf_counter_ns() - start) / 1e9
                after = resource.getrusage(resource.RUSAGE_CHILDREN)
                record = {"engine": label, "iteration": iteration, "seconds": elapsed,
                          "returncode": process.returncode, "stderr": process.stderr,
                          "minor_faults": after.ru_minflt - before.ru_minflt,
                          "major_faults": after.ru_majflt - before.ru_majflt,
                          "child_peak_rss_kib": after.ru_maxrss,
                          "output_bytes": sum(f.stat().st_size for f in output.rglob("*.parquet")),
                          "output": str(output), "command": command}
                records.append(record)
                print(f"{label} run={iteration} elapsed={elapsed:.6f}s exit={process.returncode}", flush=True)
                if process.returncode:
                    raise RuntimeError(process.stderr)
                if args.oracle:
                    checker = Path(__file__).with_name("differential.py")
                    # Isolated Python loads checker explicitly, never from dataset cwd.
                    comparisons = [r for r in records if r["iteration"] == iteration]
                    if len(comparisons) == 2:
                        code = "import runpy,sys; ns=runpy.run_path(sys.argv[1]); from pathlib import Path; ns['compare'](Path(sys.argv[2]),Path(sys.argv[3]))"
                        subprocess.run([os.sys.executable, "-I", "-c", code, str(checker.resolve()),
                                        comparisons[0]["output"], comparisons[1]["output"]], check=True)
        medians = {label: statistics.median(r["seconds"] for r in records if r["engine"] == label)
                   for label in {r["engine"] for r in records}}
    finally:
        report = {"clock": "host command invocation through successful process exit; includes launcher overhead",
                  "official_result": False, "cache": args.cache, "cpu_affinity": args.cpus,
                  "memory_limits": "24GiB no-swap container" if args.docker_image else "unconstrained native process",
                  "fault_note": "Docker child rusage measures client, not container; RSS is cumulative child high-water mark, not cgroup peak",
                  "host": subprocess.run(["uname", "-a"], capture_output=True, text=True).stdout.strip(),
                  "input": inventory, "binary_sha256": versions,
                  "generator": {"seed": args.seed, "events": args.events, "clicks": args.clicks},
                  "artifact_sha256": {name: sha256(path) for name, path in
                                     (("generator", args.generator), ("generator_archive", args.generator_archive),
                                      ("answers_archive", args.answers_archive)) if path},
                  "runs": records}
        if "medians" in locals():
            report["median_seconds"] = medians
            report["local_candidate_under_300ms"] = medians["candidate"] < 0.3
        args.report.write_text(json.dumps(report, indent=2))
    print(json.dumps(medians, indent=2))


if __name__ == "__main__":
    main()
