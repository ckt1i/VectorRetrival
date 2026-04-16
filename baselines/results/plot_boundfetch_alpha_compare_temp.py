#!/usr/bin/env python3
import csv
from pathlib import Path

import matplotlib.pyplot as plt


RESULTS_DIR = Path("/home/zcq/VDB/VectorRetrival/baselines/results")
TEMP_CSV = RESULTS_DIR / "boundfetch_alpha_compare_temp.csv"
MAIN_CSV = RESULTS_DIR / "e2e_comparison_warm.csv"
FIG_COMPARE = RESULTS_DIR / "boundfetch_alpha_compare_temp.svg"
FIG_BASELINE = RESULTS_DIR / "boundfetch_vs_baselines_temp.svg"


def load_temp_rows():
    rows = []
    with TEMP_CSV.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            row["alpha_float"] = float(row["alpha"])
            row["recall"] = float(row["recall@10"])
            row["e2e_ms_float"] = float(row["e2e_ms"])
            row["qps"] = 1000.0 / row["e2e_ms_float"]
            rows.append(row)
    return rows


def load_baselines():
    diskann = []
    faiss = []
    with MAIN_CSV.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            if row["protocol"] != "warm":
                continue
            if row["system"] not in ("DiskANN+FlatStor", "FAISS-IVFPQ+FlatStor"):
                continue
            try:
                recall = float(row["recall@10"])
                e2e_ms = float(row["e2e_ms"])
            except ValueError:
                continue
            row["recall"] = recall
            row["qps"] = 1000.0 / e2e_ms
            if row["system"] == "DiskANN+FlatStor":
                diskann.append(row)
            else:
                faiss.append(row)
    diskann.sort(key=lambda r: (r["recall"], r["qps"]))
    faiss.sort(key=lambda r: (r["recall"], r["qps"]))
    return diskann, faiss


def annotate_curve(ax, rows, color, alphas):
    lookup = {row["alpha"]: row for row in rows}
    for alpha in alphas:
        row = lookup.get(alpha)
        if row is None:
            continue
        ax.annotate(
            f"a={alpha}",
            (row["recall"], row["qps"]),
            xytext=(4, 4),
            textcoords="offset points",
            fontsize=8,
            color=color,
        )


def plot_temp_compare(rows):
    historical = sorted(
        [r for r in rows if r["mode"] == "historical_full_preload"],
        key=lambda r: r["alpha_float"],
    )
    current = sorted(
        [r for r in rows if r["mode"] == "phase1_revert_eps075"],
        key=lambda r: r["alpha_float"],
    )

    fig, ax = plt.subplots(figsize=(8.8, 5.6))
    ax.plot(
        [r["recall"] for r in historical],
        [r["qps"] for r in historical],
        marker="o",
        linewidth=2.0,
        markersize=5.5,
        color="#1f77b4",
        label="Historical full_preload",
    )
    ax.plot(
        [r["recall"] for r in current],
        [r["qps"] for r in current],
        marker="s",
        linewidth=2.2,
        markersize=5.5,
        color="#d62728",
        label="Phase1-revert eps=0.75",
    )

    annotate_curve(ax, historical, "#1f77b4", ["0.01", "0.05", "0.20"])
    annotate_curve(ax, current, "#d62728", ["0.01", "0.05", "0.20"])

    ax.set_title("BoundFetch Alpha Sweep: Historical Full-Preload vs Phase1-Revert Rebuild")
    ax.set_xlabel("Recall@10")
    ax.set_ylabel("QPS")
    ax.grid(True, linestyle="--", alpha=0.25)
    ax.legend(frameon=False)
    fig.tight_layout()
    fig.savefig(FIG_COMPARE, format="svg")
    plt.close(fig)


def plot_temp_vs_baselines(rows, diskann, faiss):
    historical = sorted(
        [r for r in rows if r["mode"] == "historical_full_preload"],
        key=lambda r: r["alpha_float"],
    )
    current = sorted(
        [r for r in rows if r["mode"] == "phase1_revert_eps075"],
        key=lambda r: r["alpha_float"],
    )

    fig, ax = plt.subplots(figsize=(8.8, 5.6))
    ax.plot(
        [r["recall"] for r in historical],
        [r["qps"] for r in historical],
        marker="o",
        linewidth=2.0,
        markersize=5.0,
        color="#1f77b4",
        label="Historical full_preload",
        alpha=0.75,
    )
    ax.plot(
        [r["recall"] for r in current],
        [r["qps"] for r in current],
        marker="s",
        linewidth=2.4,
        markersize=5.5,
        color="#d62728",
        label="Phase1-revert eps=0.75",
    )
    ax.plot(
        [r["recall"] for r in diskann],
        [r["qps"] for r in diskann],
        marker="o",
        linewidth=2.0,
        markersize=5.5,
        color="#2ca02c",
        label="DiskANN+FlatStor",
    )
    ax.plot(
        [r["recall"] for r in faiss],
        [r["qps"] for r in faiss],
        marker="^",
        linewidth=2.0,
        markersize=5.5,
        color="#9467bd",
        label="FAISS-IVFPQ+FlatStor",
    )

    current_best = next(r for r in current if r["alpha"] == "0.01")
    historical_best = next(r for r in historical if r["alpha"] == "0.01")
    ax.annotate(
        "Phase1-revert a=0.01",
        (current_best["recall"], current_best["qps"]),
        xytext=(-110, 10),
        textcoords="offset points",
        fontsize=9,
        color="#d62728",
        arrowprops={"arrowstyle": "->", "color": "#d62728", "lw": 1.0},
    )
    ax.annotate(
        "Historical full_preload a=0.01",
        (historical_best["recall"], historical_best["qps"]),
        xytext=(-10, -18),
        textcoords="offset points",
        fontsize=9,
        color="#1f77b4",
        arrowprops={"arrowstyle": "->", "color": "#1f77b4", "lw": 1.0},
    )

    ax.set_title("Phase1-Revert Pareto Curve vs Historical Full-Preload and Baselines")
    ax.set_xlabel("Recall@10")
    ax.set_ylabel("QPS")
    ax.grid(True, linestyle="--", alpha=0.25)
    ax.legend(frameon=False)
    fig.tight_layout()
    fig.savefig(FIG_BASELINE, format="svg")
    plt.close(fig)


def main():
    rows = load_temp_rows()
    diskann, faiss = load_baselines()
    plot_temp_compare(rows)
    plot_temp_vs_baselines(rows, diskann, faiss)
    print(f"Wrote {FIG_COMPARE}")
    print(f"Wrote {FIG_BASELINE}")


if __name__ == "__main__":
    main()
