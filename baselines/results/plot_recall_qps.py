#!/usr/bin/env python3
import csv
from pathlib import Path

import matplotlib.pyplot as plt


RESULTS_DIR = Path("/home/zcq/VDB/VectorRetrival/baselines/results")
CSV_PATH = RESULTS_DIR / "e2e_comparison_warm.csv"
BOUND_FETCH_FIG = RESULTS_DIR / "boundfetch_recall_qps.svg"
BASELINE_FIG = RESULTS_DIR / "baselines_recall_qps.svg"


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
            rows.append(row)
    return rows


def sort_by_recall(rows):
    return sorted(rows, key=lambda r: (r["recall"], r["qps"]))


def boundfetch_key_from_param(param: str):
    parts = [p.strip() for p in param.split(",")]
    kv = {}
    for part in parts:
        if "=" in part:
            k, v = part.split("=", 1)
            kv[k.strip()] = v.strip()
    if "nprobe" not in kv or "alpha" not in kv:
        return None
    return kv["nprobe"], kv["alpha"]


def is_full_preload(row):
    return row["system"] == "BoundFetch" and "clu_mode=full_preload" in row["param"]


def is_window_ablation(row):
    return row["system"] == "BoundFetch" and "clu_mode=window" in row["param"]


def is_historical_boundfetch(row):
    return row["system"] == "BoundFetch" and "clu_mode=" not in row["param"]


def select_boundfetch_pairs(rows):
    historical = {}
    for row in rows:
        if is_historical_boundfetch(row):
            key = boundfetch_key_from_param(row["param"])
            if key is not None:
                historical[key] = row

    window = {}
    for row in rows:
        if is_window_ablation(row):
            key = boundfetch_key_from_param(row["param"])
            if key is not None:
                window[key] = row

    preload = {}
    for row in rows:
        if is_full_preload(row):
            key = boundfetch_key_from_param(row["param"])
            if key is not None:
                preload[key] = row

    paired_original = []
    paired_preload = []
    for key, preload_row in preload.items():
        original_row = window.get(key, historical.get(key))
        if original_row is None:
            continue
        paired_original.append(original_row)
        paired_preload.append(preload_row)
    return sort_by_recall(paired_original), sort_by_recall(paired_preload)


def plot_boundfetch(rows):
    original, preload = select_boundfetch_pairs(rows)

    fig, ax = plt.subplots(figsize=(8.8, 5.6))
    ax.plot(
        [r["recall"] for r in original],
        [r["qps"] for r in original],
        marker="o",
        linewidth=2.0,
        markersize=5.5,
        color="#1f77b4",
        label="BoundFetch original / window",
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

    # Draw pairwise arrows so the direction of change is explicit.
    orig_map = {boundfetch_key_from_param(r["param"]): r for r in original}
    preload_map = {boundfetch_key_from_param(r["param"]): r for r in preload}
    for key, orig_row in orig_map.items():
        pre_row = preload_map.get(key)
        if pre_row is None:
            continue
        ax.annotate(
            "",
            xy=(pre_row["recall"], pre_row["qps"]),
            xytext=(orig_row["recall"], orig_row["qps"]),
            arrowprops={"arrowstyle": "->", "color": "#555555", "lw": 1.0, "alpha": 0.6},
        )

    key_200_005 = ("200", "0.05")
    anchor_original = orig_map[key_200_005]
    anchor_preload = preload_map[key_200_005]
    ax.annotate(
        "same point before/without preload",
        (anchor_original["recall"], anchor_original["qps"]),
        xytext=(-90, 18),
        textcoords="offset points",
        fontsize=9,
        color="#1f77b4",
        arrowprops={"arrowstyle": "->", "color": "#1f77b4", "lw": 1.0},
    )
    ax.annotate(
        "same point with full_preload",
        (anchor_preload["recall"], anchor_preload["qps"]),
        xytext=(10, -18),
        textcoords="offset points",
        fontsize=9,
        color="#d62728",
        arrowprops={"arrowstyle": "->", "color": "#d62728", "lw": 1.0},
    )

    ax.set_title("BoundFetch: Matched Original/Window vs Full-Preload")
    ax.set_xlabel("Recall@10")
    ax.set_ylabel("QPS")
    ax.grid(True, linestyle="--", alpha=0.25)
    ax.legend(frameon=False)
    fig.tight_layout()
    fig.savefig(BOUND_FETCH_FIG, format="svg")
    plt.close(fig)


def pick_boundfetch_best(rows):
    candidates = [r for r in rows if r["system"] == "BoundFetch"]
    # Use the current preload-on best tradeoff point as the representative point.
    for row in candidates:
        if row["param"] == "nlist=2048,nprobe=200,alpha=0.05,clu_mode=full_preload":
            return row
    raise RuntimeError("BoundFetch representative point not found")


def plot_baselines(rows):
    diskann = sort_by_recall([r for r in rows if r["system"] == "DiskANN+FlatStor"])
    faiss = sort_by_recall([r for r in rows if r["system"] == "FAISS-IVFPQ+FlatStor"])
    boundfetch_best = pick_boundfetch_best(rows)

    fig, ax = plt.subplots(figsize=(8.8, 5.6))
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
    ax.scatter(
        [boundfetch_best["recall"]],
        [boundfetch_best["qps"]],
        marker="D",
        s=70,
        color="#d62728",
        label="BoundFetch best tradeoff",
        zorder=5,
    )

    diskann_best_qps = max(diskann, key=lambda r: r["qps"])
    faiss_best_recall = max(faiss, key=lambda r: r["recall"])
    ax.annotate(
        "DiskANN best-QPS point",
        (diskann_best_qps["recall"], diskann_best_qps["qps"]),
        xytext=(8, 10),
        textcoords="offset points",
        fontsize=9,
        color="#2ca02c",
        arrowprops={"arrowstyle": "->", "color": "#2ca02c", "lw": 1.0},
    )
    ax.annotate(
        "FAISS recall ceiling",
        (faiss_best_recall["recall"], faiss_best_recall["qps"]),
        xytext=(-90, -18),
        textcoords="offset points",
        fontsize=9,
        color="#9467bd",
        arrowprops={"arrowstyle": "->", "color": "#9467bd", "lw": 1.0},
    )
    ax.annotate(
        "BoundFetch best tradeoff",
        (boundfetch_best["recall"], boundfetch_best["qps"]),
        xytext=(-110, 12),
        textcoords="offset points",
        fontsize=9,
        color="#d62728",
        arrowprops={"arrowstyle": "->", "color": "#d62728", "lw": 1.0},
    )

    ax.set_title("BoundFetch vs Baselines: Recall vs QPS")
    ax.set_xlabel("Recall@10")
    ax.set_ylabel("QPS")
    ax.grid(True, linestyle="--", alpha=0.25)
    ax.legend(frameon=False)
    fig.tight_layout()
    fig.savefig(BASELINE_FIG, format="svg")
    plt.close(fig)


def main():
    rows = load_rows()
    plot_boundfetch(rows)
    plot_baselines(rows)
    print(f"Wrote {BOUND_FETCH_FIG}")
    print(f"Wrote {BASELINE_FIG}")


if __name__ == "__main__":
    main()
