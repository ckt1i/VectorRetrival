#!/usr/bin/env python3
import csv
from pathlib import Path

import matplotlib.pyplot as plt


RESULTS_DIR = Path("/home/zcq/VDB/VectorRetrival/baselines/results")
CSV_PATH = RESULTS_DIR / "e2e_comparison_warm.csv"
BOUND_FETCH_FIG = RESULTS_DIR / "boundfetch_recall_qps.svg"
BASELINE_FIG = RESULTS_DIR / "baselines_recall_qps.svg"
PARETO_FIG = RESULTS_DIR / "pareto_comparison.svg"


def parse_param(param: str):
    kv = {}
    for part in param.split(","):
        part = part.strip()
        if "=" not in part:
            continue
        k, v = part.split("=", 1)
        kv[k.strip()] = v.strip()
    return kv


def load_rows():
    rows = []
    with CSV_PATH.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            if row["protocol"] != "warm":
                continue
            try:
                row["recall"] = float(row["recall@10"])
                row["e2e_ms_float"] = float(row["e2e_ms"])
            except ValueError:
                continue
            row["qps"] = 1000.0 / row["e2e_ms_float"] if row["e2e_ms_float"] > 0 else 0.0
            row["param_kv"] = parse_param(row["param"])
            rows.append(row)
    return rows


def select_historical_full_preload(rows):
    selected = []
    for row in rows:
        kv = row["param_kv"]
        if row["system"] != "BoundFetch":
            continue
        if kv.get("clu_mode") != "full_preload":
            continue
        if "epsilon" in kv or "index" in kv:
            continue
        if kv.get("nprobe") != "200":
            continue
        if not row["notes"].startswith("low-load retest"):
            continue
        selected.append(row)
    return sorted(selected, key=lambda r: (r["recall"], r["qps"]))


def select_current_rebuild(rows):
    selected = []
    for row in rows:
        kv = row["param_kv"]
        if row["system"] != "BoundFetch":
            continue
        if kv.get("clu_mode") != "full_preload":
            continue
        if kv.get("epsilon") != "0.90":
            continue
        if kv.get("bits") != "4":
            continue
        if kv.get("index") != "eps090_nprobe512_20260416":
            continue
        if kv.get("nprobe") != "512":
            continue
        selected.append(row)
    return sorted(selected, key=lambda r: (r["recall"], r["qps"]))


def select_baselines(rows):
    diskann = sorted(
        [r for r in rows if r["system"] == "DiskANN+FlatStor"],
        key=lambda r: (r["recall"], r["qps"]),
    )
    faiss = sorted(
        [r for r in rows if r["system"] == "FAISS-IVFPQ+FlatStor"],
        key=lambda r: (r["recall"], r["qps"]),
    )
    return diskann, faiss


def annotate_curve(ax, rows, color, alphas):
    lookup = {r["param_kv"].get("alpha"): r for r in rows}
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


def plot_boundfetch(rows):
    historical = select_historical_full_preload(rows)
    current = select_current_rebuild(rows)

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
        label="eps=0.90, nprobe=512",
    )

    annotate_curve(ax, historical, "#1f77b4", ["0.01", "0.05", "0.20"])
    annotate_curve(ax, current, "#d62728", ["0.01", "0.08", "0.20"])

    ax.set_title("BoundFetch Pareto Curves: Historical Full-Preload vs eps=0.90, nprobe=512")
    ax.set_xlabel("Recall@10")
    ax.set_ylabel("QPS")
    ax.grid(True, linestyle="--", alpha=0.25)
    ax.legend(frameon=False)
    fig.tight_layout()
    fig.savefig(BOUND_FETCH_FIG, format="svg")
    plt.close(fig)


def plot_baselines(rows):
    historical = select_historical_full_preload(rows)
    current = select_current_rebuild(rows)
    diskann, faiss = select_baselines(rows)

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
        label="BoundFetch eps=0.90, nprobe=512",
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

    current_best = next(r for r in current if r["param_kv"].get("alpha") == "0.01")
    historical_best = next(r for r in historical if r["param_kv"].get("alpha") == "0.01")
    ax.annotate(
        "BoundFetch eps=0.90, nprobe=512, a=0.01",
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

    ax.set_title("Pareto Comparison: eps=0.90, nprobe=512 vs Historical Full-Preload and Baselines")
    ax.set_xlabel("Recall@10")
    ax.set_ylabel("QPS")
    ax.grid(True, linestyle="--", alpha=0.25)
    ax.legend(frameon=False)
    fig.tight_layout()
    fig.savefig(BASELINE_FIG, format="svg")
    fig.savefig(PARETO_FIG, format="svg")
    plt.close(fig)


def main():
    rows = load_rows()
    plot_boundfetch(rows)
    plot_baselines(rows)
    print(f"Wrote {BOUND_FETCH_FIG}")
    print(f"Wrote {BASELINE_FIG}")
    print(f"Wrote {PARETO_FIG}")


if __name__ == "__main__":
    main()
