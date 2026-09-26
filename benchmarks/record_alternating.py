"""Compare two lock-free benchmark binaries in alternating order."""

import csv
import json
import re
import statistics
import subprocess
import sys
from pathlib import Path

ITEMS = 3_000_000
THIEVES = (0, 1, 2, 4, 8)
TRIALS = 10
RESULT = re.compile(r"lock-free seconds=([0-9.]+).* (PASS)$", re.MULTILINE)


def run(binary: Path, thieves: int) -> float:
    output = subprocess.check_output(
        [str(binary), str(ITEMS), str(thieves)], text=True
    )
    match = RESULT.search(output)
    if not match:
        raise RuntimeError(f"missing valid result from {binary}:\n{output}")
    return float(match.group(1))


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit("Usage: record_alternating.py REFERENCE CANDIDATE OUTPUT_DIR")
    reference, candidate, output = (Path(arg).resolve() for arg in sys.argv[1:])
    output.mkdir(parents=True, exist_ok=True)
    rows = []
    summary = {}
    for thieves in THIEVES:
        run(reference, thieves)
        run(candidate, thieves)
        for trial in range(TRIALS):
            order = (("reference", reference), ("candidate", candidate))
            if trial % 2:
                order = tuple(reversed(order))
            for position, (name, binary) in enumerate(order):
                rows.append((thieves, trial + 1, position + 1, name,
                             run(binary, thieves)))
        medians = {
            name: statistics.median(row[4] for row in rows
                                    if row[0] == thieves and row[3] == name)
            for name in ("reference", "candidate")
        }
        summary[thieves] = medians
        print(f"thieves={thieves}: reference={medians['reference']:.6f}s "
              f"candidate={medians['candidate']:.6f}s", flush=True)
    with (output / "alternating.csv").open("w", newline="") as file:
        writer = csv.writer(file)
        writer.writerow(("thieves", "trial", "position", "implementation", "seconds"))
        writer.writerows(rows)
    (output / "alternating-summary.json").write_text(
        json.dumps({"items": ITEMS, "trials_per_count": TRIALS,
                    "reference": str(reference), "candidate": str(candidate),
                    "medians": summary}, indent=2) + "\n"
    )


if __name__ == "__main__":
    main()
