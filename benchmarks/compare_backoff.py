"""Pair the previous steal loop with backoff; reuse saved mutex timings."""

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
MUTEX_SUMMARY = ROOT / "benchmark-results/weaker-orders-stage-1/summary.csv"
RESULT = re.compile(
    r"^lock-free seconds=([0-9.]+) Mitems/s=[0-9.]+ "
    r"owner=(\d+) stolen=(\d+) (PASS|FAIL)$"
)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def output(*args):
    return subprocess.check_output(args, cwd=ROOT, text=True).strip()


def benchmark(binary, thieves, mode):
    command = [str(binary), str(ITEMS), str(thieves)]
    if mode:
        command.append(mode)
    stdout = output(*command)
    matches = [RESULT.fullmatch(line) for line in stdout.splitlines()]
    matches = [match for match in matches if match]
    if len(matches) != 1:
        raise RuntimeError(f"unexpected output from {binary}:\n{stdout}")
    seconds, owner, stolen, status = matches[0].groups()
    if status != "PASS" or int(owner) + int(stolen) != ITEMS:
        raise RuntimeError(f"invalid result from {binary}:\n{stdout}")
    return stdout, seconds, owner, stolen


def main():
    if len(sys.argv) != 4:
        raise SystemExit(
            "Usage: python3 benchmarks/compare_backoff.py "
            "PREVIOUS_BINARY BACKOFF_BINARY OUTPUT_DIRECTORY"
        )
    previous, backoff, destination = (Path(arg).resolve() for arg in sys.argv[1:])
    with MUTEX_SUMMARY.open(newline="") as csv_file:
        mutex = {int(row["thieves"]): row for row in csv.DictReader(csv_file)}
    if set(mutex) != set(THIEF_COUNTS) or any(
        int(row["items"]) != ITEMS for row in mutex.values()
    ):
        raise RuntimeError("saved mutex summary does not match the workload")

    destination.mkdir(parents=True, exist_ok=False)
    metadata = {
        "recorded_utc": datetime.now(timezone.utc).isoformat(),
        "git_commit": output("git", "rev-parse", "HEAD"),
        "git_status": output("git", "status", "--short"),
        "platform": platform.platform(),
        "logical_cpus": os.cpu_count(),
        "compiler": output("g++", "--version").splitlines()[0],
        "items": ITEMS,
        "thief_counts": THIEF_COUNTS,
        "warmups_per_variant_per_count": WARMUPS,
        "measured_pairs_per_count": TRIALS,
        "order": "previous then backoff on odd trials; reversed on even trials",
        "previous": str(previous),
        "backoff": str(backoff),
        "backoff_mode": "backoff (default configuration: 4 attempts, window 4..256)",
        "mutex_source": str(MUTEX_SUMMARY.relative_to(ROOT)),
        "mutex_measurement": "saved median only; mutex binary was not run",
        "sha256": {
            "previous_binary": sha256(previous),
            "backoff_binary": sha256(backoff),
            "previous_source": sha256(ROOT / "build/benchmark_previous.cpp"),
            "backoff_source": sha256(ROOT / "test/benchmark_deque.cpp"),
            "backoff_header": sha256(ROOT / "src/steal_backoff.h"),
            "deque_header": sha256(ROOT / "src/work_stealing_deque.h"),
            "mutex_summary": sha256(MUTEX_SUMMARY),
            "collector": sha256(Path(__file__)),
        },
    }
    (destination / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")

    rows = []
    variants = (("reference", previous, None), ("candidate", backoff, "backoff"))
    with (destination / "runs.log").open("w") as log:
        for thieves in THIEF_COUNTS:
            for variant, binary, mode in variants:
                for warmup in range(1, WARMUPS + 1):
                    stdout, _, _, _ = benchmark(binary, thieves, mode)
                    log.write(f"thieves={thieves} {variant} warmup={warmup}\n{stdout}\n")
                    log.flush()
            for trial in range(1, TRIALS + 1):
                order = variants if trial % 2 else variants[::-1]
                for variant, binary, mode in order:
                    stdout, seconds, owner, stolen = benchmark(binary, thieves, mode)
                    log.write(f"thieves={thieves} trial={trial} {variant}\n{stdout}\n")
                    log.flush()
                    rows.append((ITEMS, thieves, trial, variant, seconds,
                                 owner, stolen, "PASS"))
            print(f"completed {thieves} thieves", flush=True)

    with (destination / "raw.csv").open("w", newline="") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(("items", "thieves", "trial", "variant", "seconds",
                         "owner", "stolen", "status"))
        writer.writerows(rows)

    with (destination / "summary.csv").open("w", newline="") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(("items", "thieves", "reference_median_seconds",
                         "candidate_median_seconds", "candidate_time_reduction_percent",
                         "mutex_median_seconds", "candidate_vs_mutex_time_reduction_percent",
                         "reference_min_seconds", "reference_max_seconds",
                         "candidate_min_seconds", "candidate_max_seconds"))
        for thieves in THIEF_COUNTS:
            times = {
                variant: [float(row[4]) for row in rows
                          if row[1] == thieves and row[3] == variant]
                for variant in ("reference", "candidate")
            }
            if any(len(values) != TRIALS for values in times.values()):
                raise RuntimeError(f"missing measured trials for {thieves} thieves")
            old = statistics.median(times["reference"])
            new = statistics.median(times["candidate"])
            mutex_time = float(mutex[thieves]["mutex_median_seconds"])
            writer.writerow((ITEMS, thieves, f"{old:.6f}", f"{new:.6f}",
                             f"{100 * (old - new) / old:.1f}",
                             f"{mutex_time:.6f}",
                             f"{100 * (mutex_time - new) / mutex_time:.1f}",
                             f"{min(times['reference']):.6f}",
                             f"{max(times['reference']):.6f}",
                             f"{min(times['candidate']):.6f}",
                             f"{max(times['candidate']):.6f}"))
            print(f"thieves={thieves}: previous={old:.6f}s "
                  f"backoff={new:.6f}s mutex(saved)={mutex_time:.6f}s",
                  flush=True)


if __name__ == "__main__":
    main()
