"""Record a repeatable baseline for the deque and mutex benchmark."""

import csv
import hashlib
import json
import os
import platform
import re
import statistics
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ITEMS = 3_000_000
THIEF_COUNTS = (0, 1, 2, 4, 8)
WARMUPS = 2
TRIALS = 10
SOURCES = (
    "test/benchmark_deque.cpp",
    "src/work_stealing_deque.h",
    "src/circular_array.h",
    "src/atomic_utils.h",
    "src/buffer_pool.h",
    "src/steal_result.h",
    "src/mutex/work_stealing_deque.h",
    "Makefile",
    "benchmarks/record_comparison.py",
)
RESULT = re.compile(
    r"^(lock-free|mutex) seconds=([0-9.]+) Mitems/s=([0-9.]+) "
    r"owner=(\d+) stolen=(\d+) (PASS|FAIL)$"
)


def output(*args):
    return subprocess.check_output(args, cwd=ROOT, text=True).strip()


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(thieves):
    command = [str(ROOT / "build/benchmark_deque"), str(ITEMS), str(thieves), "compare"]
    stdout = output(*command)
    results = []
    for line in stdout.splitlines():
        if match := RESULT.fullmatch(line):
            name, seconds, throughput, owner, stolen, status = match.groups()
            if status != "PASS" or int(owner) + int(stolen) != ITEMS:
                raise RuntimeError(f"invalid benchmark result: {line}")
            results.append((name, seconds, throughput, owner, stolen, status))
    if [row[0] for row in results] != ["lock-free", "mutex"]:
        raise RuntimeError(f"unexpected benchmark output:\n{stdout}")
    return stdout, results


def write_summary(destination):
    with (destination / "raw.csv").open(newline="") as csv_file:
        rows = list(csv.DictReader(csv_file))

    with (destination / "summary.csv").open("w", newline="") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(("items", "thieves", "lock_free_median_seconds",
                         "lock_free_average_seconds", "mutex_median_seconds",
                         "mutex_average_seconds", "mutex_over_lock_free_time"))
        for thieves in THIEF_COUNTS:
            times = {
                name: [float(row["seconds"]) for row in rows
                       if int(row["thieves"]) == thieves and
                       row["implementation"] == name and row["status"] == "PASS"]
                for name in ("lock-free", "mutex")
            }
            if any(len(values) != TRIALS for values in times.values()):
                raise RuntimeError(f"missing valid measured trials for {thieves} thieves")
            lock_free = statistics.median(times["lock-free"])
            mutex = statistics.median(times["mutex"])
            writer.writerow((ITEMS, thieves, f"{lock_free:.6f}",
                             f"{statistics.mean(times['lock-free']):.6f}",
                             f"{mutex:.6f}",
                             f"{statistics.mean(times['mutex']):.6f}",
                             f"{mutex / lock_free:.3f}"))
            print(f"thieves={thieves}: lock-free={lock_free:.6f}s "
                  f"mutex={mutex:.6f}s mutex/lock-free={mutex / lock_free:.3f}x",
                  flush=True)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Usage: python3 benchmarks/record_comparison.py OUTPUT_DIRECTORY")
    subprocess.run(["make", "build/benchmark_deque"], cwd=ROOT, check=True)
    destination = Path(sys.argv[1]).resolve()
    destination.mkdir(parents=True, exist_ok=False)

    metadata = {
        "recorded_utc": datetime.now(timezone.utc).isoformat(),
        "git_commit": output("git", "rev-parse", "HEAD"),
        "git_status": output("git", "status", "--short"),
        "platform": platform.platform(),
        "logical_cpus": os.cpu_count(),
        "compiler": output("g++", "--version").splitlines()[0],
        "build_target": "make build/benchmark_deque",
        "command": "./build/benchmark_deque ITEMS THIEVES compare",
        "items": ITEMS,
        "thief_counts": THIEF_COUNTS,
        "warmups_per_count": WARMUPS,
        "measured_trials_per_count": TRIALS,
        "comparison_order": ["lock-free", "mutex"],
        "timing": "Owner pushes while thieves steal; owner drains after join. "
                  "Worker setup and validation are outside the timer.",
        "sha256": {name: sha256(ROOT / name) for name in SOURCES}
                  | {"build/benchmark_deque": sha256(ROOT / "build/benchmark_deque")},
    }
    (destination / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")

    rows = []
    with (destination / "runs.log").open("w") as log:
        for thieves in THIEF_COUNTS:
            for warmup in range(1, WARMUPS + 1):
                stdout, _ = run(thieves)
                log.write(f"thieves={thieves} warmup={warmup}\n{stdout}\n")
            for trial in range(1, TRIALS + 1):
                stdout, results = run(thieves)
                log.write(f"thieves={thieves} trial={trial}\n{stdout}\n")
                for name, seconds, throughput, owner, stolen, status in results:
                    rows.append((ITEMS, thieves, trial, name, seconds, throughput,
                                 owner, stolen, status))

    with (destination / "raw.csv").open("w", newline="") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(("items", "thieves", "trial", "implementation", "seconds",
                         "mitems_per_second", "owner", "stolen", "status"))
        writer.writerows(rows)

    write_summary(destination)


if __name__ == "__main__":
    main()
