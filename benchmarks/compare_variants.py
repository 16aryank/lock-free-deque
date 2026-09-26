"""Compare two builds with alternating run order and the standard workload."""

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
RESULT = re.compile(
    r"^lock-free seconds=([0-9.]+) Mitems/s=[0-9.]+ "
    r"owner=(\d+) stolen=(\d+) (PASS|FAIL)$"
)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def benchmark(binary, thieves):
    command = [str(binary), str(ITEMS), str(thieves)]
    stdout = subprocess.check_output(command, cwd=ROOT, text=True).strip()
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
            "Usage: python3 benchmarks/compare_variants.py REFERENCE CANDIDATE OUTPUT_DIRECTORY"
        )
    reference, candidate, destination = (Path(arg).resolve() for arg in sys.argv[1:])
    destination.mkdir(parents=True, exist_ok=False)
    metadata = {
        "recorded_utc": datetime.now(timezone.utc).isoformat(),
        "git_commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
        ).strip(),
        "git_status": subprocess.check_output(
            ["git", "status", "--short"], cwd=ROOT, text=True
        ).strip(),
        "platform": platform.platform(),
        "logical_cpus": os.cpu_count(),
        "compiler": subprocess.check_output(
            ["g++", "--version"], cwd=ROOT, text=True
        ).splitlines()[0],
        "items": ITEMS,
        "thief_counts": THIEF_COUNTS,
        "warmups_per_variant_per_count": WARMUPS,
        "measured_pairs_per_count": TRIALS,
        "order": "reference then candidate on odd trials; reversed on even trials",
        "reference": str(reference),
        "candidate": str(candidate),
        "sha256": {
            "reference_binary": sha256(reference),
            "candidate_binary": sha256(candidate),
            "benchmark_source": sha256(ROOT / "test/benchmark_deque.cpp"),
            "candidate_header": sha256(ROOT / "src/work_stealing_deque.h"),
            "collector": sha256(Path(__file__)),
        },
    }
    (destination / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")

    rows = []
    with (destination / "runs.log").open("w") as log:
        for thieves in THIEF_COUNTS:
            for variant, binary in (("reference", reference), ("candidate", candidate)):
                for warmup in range(1, WARMUPS + 1):
                    stdout, _, _, _ = benchmark(binary, thieves)
                    log.write(f"thieves={thieves} {variant} warmup={warmup}\n{stdout}\n")
            for trial in range(1, TRIALS + 1):
                order = (("reference", reference), ("candidate", candidate))
                if trial % 2 == 0:
                    order = order[::-1]
                for variant, binary in order:
                    stdout, seconds, owner, stolen = benchmark(binary, thieves)
                    log.write(f"thieves={thieves} trial={trial} {variant}\n{stdout}\n")
                    rows.append((ITEMS, thieves, trial, variant, seconds, owner, stolen, "PASS"))
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
                         "reference_min_seconds", "reference_max_seconds",
                         "candidate_min_seconds", "candidate_max_seconds"))
        for thieves in THIEF_COUNTS:
            times = {
                variant: [float(row[4]) for row in rows
                          if row[1] == thieves and row[3] == variant]
                for variant in ("reference", "candidate")
            }
            old = statistics.median(times["reference"])
            new = statistics.median(times["candidate"])
            reduction = 100 * (old - new) / old
            writer.writerow((ITEMS, thieves, f"{old:.6f}", f"{new:.6f}",
                             f"{reduction:.1f}", f"{min(times['reference']):.6f}",
                             f"{max(times['reference']):.6f}",
                             f"{min(times['candidate']):.6f}",
                             f"{max(times['candidate']):.6f}"))
            print(f"thieves={thieves}: reference={old:.6f}s candidate={new:.6f}s "
                  f"reduction={reduction:.1f}%", flush=True)


if __name__ == "__main__":
    main()
