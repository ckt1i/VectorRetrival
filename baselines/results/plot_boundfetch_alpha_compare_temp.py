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
            row["e2e_ms"] = float(row["e2e_ms"])
            row["qps"] = 1000.0 / row["e2e_ms"]
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
    return diskann, faiss


def plot_temp_compare(rows):
    window = sorted([r for r in rows if r["mode"] == "window"], key=lambda r: r["alpha_float"])
    preload = sorted([r for r in rows if r["mode"] == "full_preload"], key=lambda r: r["alpha_float"])

    fig, ax = plt.subplots(figsize=(8.8, 5.6))
    ax.plot(
        [r["recall"] for r in window],
        [r["qps"] for r in window],
        marker="o",
        linewidth=2.0,
        markersize=5.5,
        color="#1f77b4",
        label="BoundFetch window",
    )
    ax.plot(
        [r["recall"] for r in preload],
        [r["qps"] for r in preload],
        marker="s",
        linewidth=2.0,
        markersize=5.5,
        color="#d62728",
        label="BoundFetch full_preload",
    )

    preload_map = {r["alpha"]: r for r in preload}
    for row in window:
        paired = preload_map[row["alpha"]]
        ax.annotate(
            "",
            xy=(paired["recall"], paired["qps"]),
            xytext=(row["recall"], row["qps"]),
            arrowprops={"arrowstyle": "->", "color": "#666666", "lw": 1.0, "alpha": 0.7},
        )
        ax.annotate(
            f"a={row['alpha']}",
            (paired["recall"], paired["qps"]),
            xytext=(4, 4),
            textcoords="offset points",
            fontsize=8,
            color="#444444",
        )

    ax.set_title("BoundFetch Alpha Sweep at nprobe=200: Window vs Full-Preload")
    ax.set_xlabel("Recall@10")
    ax.set_ylabel("QPS")
    ax.grid(True, linestyle="--", alpha=0.25)
    ax.legend(frameon=False)
    fig.tight_layout()
    fig.savefig(FIG_COMPARE, format="svg")
    plt.close(fig)


def plot_temp_vs_baselines(rows, diskann, faiss):
    window = sorted([r for r in rows if r["mode"] == "window"], key=lambda r: r["alpha_float"])
    preload = sorted([r for r in rows if r["mode"] == "full_preload"], key=lambda r: r["alpha_float"])

    fig, ax = plt.subplots(figsize=(8.8, 5.6))
    ax.plot(
        [r["recall"] for r in window],
        [r["qps"] for r in window],
        marker="o",
        linewidth=2.0,
        markersize=5.0,
        color="#1f77b4",
        label="BoundFetch window",
        alpha=0.55,
    )
    ax.plot(
        [r["recall"] for r in preload],
        [r["qps"] for r in preload],
        marker="s",
        linewidth=2.4,
        markersize=5.5,
        color="#d62728",
        label="BoundFetch full_preload",
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

    bf_best = next(r for r in preload if r["alpha"] == "0.05")
    ax.annotate(
        "BoundFetch a=0.05",
        (bf_best["recall"], bf_best["qps"]),
        xytext=(-95, 10),
        textcoords="offset points",
        fontsize=9,
        color="#d62728",
        arrowprops={"arrowstyle": "->", "color": "#d62728", "lw": 1.0},
    )

    ax.set_title("Temporary Comparison: BoundFetch Curve vs Baselines")
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
