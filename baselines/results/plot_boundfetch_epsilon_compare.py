#!/usr/bin/env python3
import csv
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt


RESULTS_DIR = Path("/home/zcq/VDB/VectorRetrival/baselines/results")
CSV_PATH = RESULTS_DIR / "boundfetch_epsilon_alpha_sweep.csv"
FIG_PATH = RESULTS_DIR / "boundfetch_epsilon_pareto.svg"


def load_rows():
    rows = []
    with CSV_PATH.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            row["epsilon_float"] = float(row["epsilon"])
            row["alpha_float"] = float(row["alpha"])
            row["recall"] = float(row["recall@10"])
            row["e2e_ms_float"] = float(row["e2e_ms"])
            row["qps"] = 1000.0 / row["e2e_ms_float"]
            rows.append(row)
    return rows


def grouped_rows(rows):
    groups = defaultdict(list)
    for row in rows:
        groups[row["epsilon"]].append(row)
    for epsilon in groups:
        groups[epsilon].sort(key=lambda r: (r["recall"], r["qps"]))
    return dict(sorted(groups.items(), key=lambda kv: float(kv[0])))


def main():
    rows = load_rows()
    groups = grouped_rows(rows)
    colors = {
        "0.75": "#d62728",
        "0.80": "#ff7f0e",
        "0.90": "#2ca02c",
        "0.95": "#1f77b4",
        "0.99": "#9467bd",
    }

    fig, ax = plt.subplots(figsize=(9.2, 5.8))
    for epsilon, group in groups.items():
        color = colors.get(epsilon, None)
        ax.plot(
            [r["recall"] for r in group],
            [r["qps"] for r in group],
            marker="o",
            linewidth=2.1,
            markersize=5.0,
            color=color,
            label=f"epsilon={epsilon}",
        )

        alpha_lookup = {r["alpha"]: r for r in group}
        for alpha in ("0.01", "0.05", "0.20"):
            row = alpha_lookup.get(alpha)
            if row is None:
                continue
            ax.annotate(
                f"{epsilon}/a={alpha}",
                (row["recall"], row["qps"]),
                xytext=(4, 4),
                textcoords="offset points",
                fontsize=7.5,
                color=color,
            )

    ax.set_title("BoundFetch Pareto Curves Across Epsilon Percentiles")
    ax.set_xlabel("Recall@10")
    ax.set_ylabel("QPS")
    ax.grid(True, linestyle="--", alpha=0.25)
    ax.legend(frameon=False, ncol=2)
    fig.tight_layout()
    fig.savefig(FIG_PATH, format="svg")
    plt.close(fig)
    print(f"Wrote {FIG_PATH}")


if __name__ == "__main__":
    main()
