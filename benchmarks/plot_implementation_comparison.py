"""Plot saved median runtimes for the four deque implementations."""

import csv
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import MaxNLocator


ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "benchmark-results"
OUTPUT = RESULTS / "implementation-comparison"
THIEF_COUNTS = (0, 1, 2, 4, 8)
ITEMS = 3_000_000
SERIES = (
    ("Mutex", "weaker-orders-stage-1/summary.csv", "mutex_median_seconds", "#65758B"),
    ("Sequential consistency", "2026-09-25-seq-cst-baseline/summary.csv", "lock_free_median_seconds", "#7955A0"),
    ("Cache layout", "2026-09-26-cache-layout-p1/summary.csv", "candidate_median_seconds", "#168E91"),
    ("Steal backoff", "2026-09-29-steal-backoff/summary.csv", "candidate_median_seconds", "#E06A3B"),
)


def read_times(filename, column):
    with (RESULTS / filename).open(newline="") as csv_file:
        rows = list(csv.DictReader(csv_file))
    times = {int(row["thieves"]): float(row[column]) for row in rows}
    if (set(times) != set(THIEF_COUNTS) or len(rows) != len(THIEF_COUNTS)
            or any(int(row["items"]) != ITEMS for row in rows)):
        raise ValueError(f"unexpected workload or thief counts in {filename}")
    return times


def main():
    data = [(name, read_times(filename, column), color)
            for name, filename, column, color in SERIES]
    plt.rcParams.update({
        "font.family": "DejaVu Sans",
        "font.size": 10,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "axes.spines.left": False,
        "axes.edgecolor": "#B8C0C9",
        "axes.labelcolor": "#344052",
        "text.color": "#243142",
        "xtick.color": "#596679",
        "ytick.color": "#243142",
        "svg.fonttype": "none",
    })
    fig, axes = plt.subplots(len(THIEF_COUNTS), 1, figsize=(11.5, 13.5))
    fig.patch.set_facecolor("white")
    fig.suptitle("Work-stealing deque performance", x=0.15, y=0.985,
                 ha="left", fontsize=19, fontweight="bold")
    fig.text(0.15, 0.959,
             "Median runtime for 3 million items · lower is faster",
             ha="left", fontsize=11, color="#596679")

    for ax, thieves in zip(axes, THIEF_COUNTS):
        values = [times[thieves] for _, times, _ in data]
        scale = max(values)
        for index, ((name, _, color), value) in enumerate(zip(data, values)):
            ax.barh(index, value, height=0.67, color=color)
            ax.text(value + scale * 0.018, index, f"{value:.3f} s",
                    va="center", ha="left", fontsize=10, fontweight="bold")
        ax.set_yticks(range(len(data)), [name for name, _, _ in data])
        ax.invert_yaxis()
        ax.set_xlim(0, scale * 1.25)
        ax.xaxis.set_major_locator(MaxNLocator(nbins=5, min_n_ticks=3))
        ax.grid(axis="x", color="#E6EAEE", linewidth=0.8)
        ax.set_axisbelow(True)
        ax.tick_params(axis="y", length=0, pad=12)
        ax.tick_params(axis="x", length=0, pad=5)
        title = "0 thieves · owner only" if thieves == 0 else (
            "1 thief" if thieves == 1 else f"{thieves} thieves"
        )
        ax.set_title(title,
                     loc="left", pad=9, fontsize=12, fontweight="bold")
        ax.set_xlabel("Median runtime (seconds)", labelpad=6, fontsize=9)

    fig.subplots_adjust(left=0.24, right=0.91, top=0.91, bottom=0.09, hspace=0.72)
    fig.text(0.15, 0.038,
             "Each panel has its own time scale. Results were saved in separate sessions; "
             "the mutex times are historical.",
             ha="left", fontsize=9, color="#596679")
    for suffix in ("svg", "png"):
        fig.savefig(OUTPUT.with_suffix(f".{suffix}"), dpi=180, facecolor="white")
    plt.close(fig)


if __name__ == "__main__":
    main()
