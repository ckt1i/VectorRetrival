#!/usr/bin/env python3
"""Plot RaBitQ diagnostic benchmark results.

Reads CSV files produced by bench_rabitq_diagnostic and generates 6 plots:
  1. Scatter: est_dist vs exact_dist
  2. kth convergence curves
  3. d_min/d_max comparison
  4. Classification confusion heatmaps
  5. False SafeOut rate by margin quartile
  6. Top-K overlap curve

Usage:
    python plot_rabitq_diagnostic.py --dir ./diag_output/coco_1k [--save]
"""

import argparse
import os
import sys

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import seaborn as sns


def plot_scatter(df, ax):
    """Plot 1: est_dist vs exact_dist, colored by is_true_topk."""
    nn = df[df["is_true_topk"] == 1]
    other = df[df["is_true_topk"] == 0]

    ax.scatter(other["exact_dist"], other["est_dist"],
               s=1, alpha=0.1, c="gray", label="non-NN", rasterized=True)
    ax.scatter(nn["exact_dist"], nn["est_dist"],
               s=8, alpha=0.8, c="red", label="true NN", zorder=5)

    # y=x reference line
    lims = [
        min(ax.get_xlim()[0], ax.get_ylim()[0]),
        max(ax.get_xlim()[1], ax.get_ylim()[1]),
    ]
    ax.plot(lims, lims, "k--", alpha=0.3, lw=1)
    ax.set_xlabel("Exact L2² distance")
    ax.set_ylabel("RaBitQ estimated distance")
    ax.set_title("RaBitQ Estimate vs Exact Distance")
    ax.legend(loc="upper left", fontsize=8)


def plot_kth_convergence(df, ax):
    """Plot 2: kth convergence curves with P25-P75 bands."""
    grouped = df.groupby("probed_count")

    steps = sorted(df["probed_count"].unique())
    exact_mean = [grouped.get_group(s)["exact_kth"].replace([np.inf], np.nan).mean() for s in steps]
    est_mean = [grouped.get_group(s)["est_kth"].replace([np.inf], np.nan).mean() for s in steps]
    exact_p25 = [grouped.get_group(s)["exact_kth"].replace([np.inf], np.nan).quantile(0.25) for s in steps]
    exact_p75 = [grouped.get_group(s)["exact_kth"].replace([np.inf], np.nan).quantile(0.75) for s in steps]
    est_p25 = [grouped.get_group(s)["est_kth"].replace([np.inf], np.nan).quantile(0.25) for s in steps]
    est_p75 = [grouped.get_group(s)["est_kth"].replace([np.inf], np.nan).quantile(0.75) for s in steps]

    ax.plot(steps, exact_mean, "b-", label="Exact kth", linewidth=2)
    ax.fill_between(steps, exact_p25, exact_p75, alpha=0.2, color="blue")
    ax.plot(steps, est_mean, "r-", label="RaBitQ kth", linewidth=2)
    ax.fill_between(steps, est_p25, est_p75, alpha=0.2, color="red")

    ax.set_xlabel("Clusters probed")
    ax.set_ylabel("kth distance (L2²)")
    ax.set_title("kth-Distance Convergence: Exact vs RaBitQ")
    ax.legend(fontsize=9)
    ax.grid(True, alpha=0.3)


def plot_dmin_dmax(df, ax):
    """Plot 3: d_min/d_max comparison bar chart."""
    finite_exact = df["exact_kth"].replace([np.inf, -np.inf], np.nan).dropna()
    finite_est = df["est_kth"].replace([np.inf, -np.inf], np.nan).dropna()

    exact_min = finite_exact.min()
    exact_max = finite_exact.max()
    est_min = finite_est.min()
    est_max = finite_est.max()

    x = np.arange(2)
    width = 0.35

    bars_min = ax.bar(x - width / 2, [exact_min, est_min], width,
                      label="d_min", color=["#4477AA", "#CC6677"])
    bars_max = ax.bar(x + width / 2, [exact_max, est_max], width,
                      label="d_max", color=["#4477AA", "#CC6677"], alpha=0.5)

    ax.set_xticks(x)
    ax.set_xticklabels(["Exact L2", "RaBitQ Est"])
    ax.set_ylabel("Distance (L2²)")
    ax.set_title("d_min / d_max: Normalization Range")
    ax.legend(fontsize=9)

    # Annotate ranges
    exact_range = exact_max - exact_min
    est_range = est_max - est_min
    ratio = est_range / exact_range if exact_range > 0 else 0
    ax.text(0.5, 0.95, f"Range ratio: {ratio:.2f}x",
            transform=ax.transAxes, ha="center", va="top",
            fontsize=10, bbox=dict(boxstyle="round", facecolor="wheat", alpha=0.5))


def plot_confusion(df, axes):
    """Plot 4: Classification confusion heatmaps."""
    classes = ["SafeIn", "SafeOut", "Uncertain"]

    for ax, est_col, title in zip(
        axes,
        ["class_est", "class_est_adaptive"],
        ["Classify(est_dist)", "ClassifyAdaptive(est_dist)"],
    ):
        mat = np.zeros((3, 3), dtype=int)
        for i, ce in enumerate(classes):
            for j, cs in enumerate(classes):
                mat[i, j] = len(df[(df["class_exact"] == ce) & (df[est_col] == cs)])

        # Normalize per row
        row_sums = mat.sum(axis=1, keepdims=True)
        mat_pct = np.divide(mat, row_sums, where=row_sums > 0,
                            out=np.zeros_like(mat, dtype=float)) * 100

        sns.heatmap(mat_pct, annot=True, fmt=".1f", cmap="YlOrRd",
                    xticklabels=classes, yticklabels=classes, ax=ax,
                    vmin=0, vmax=100, cbar_kws={"label": "%"})
        ax.set_xlabel(f"Predicted ({title})")
        ax.set_ylabel("True (Exact L2)")
        ax.set_title(f"Confusion: {title}")


def plot_false_safeout_by_margin(df, ax):
    """Plot 5: False SafeOut rate by margin quartile."""
    nn_df = df[df["is_true_topk"] == 1].copy()
    if len(nn_df) == 0:
        ax.text(0.5, 0.5, "No true NN data", ha="center", va="center",
                transform=ax.transAxes)
        return

    nn_df["margin_q"] = pd.qcut(nn_df["margin"], q=4, labels=["Q1", "Q2", "Q3", "Q4"],
                                 duplicates="drop")

    rates_est = []
    rates_adp = []
    labels = []
    for q in nn_df["margin_q"].unique():
        sub = nn_df[nn_df["margin_q"] == q]
        n = len(sub)
        if n == 0:
            continue
        labels.append(str(q))
        rates_est.append(100.0 * (sub["class_est"] == "SafeOut").sum() / n)
        rates_adp.append(100.0 * (sub["class_est_adaptive"] == "SafeOut").sum() / n)

    x = np.arange(len(labels))
    width = 0.35
    ax.bar(x - width / 2, rates_est, width, label="Classify(est)", color="#4477AA")
    ax.bar(x + width / 2, rates_adp, width, label="ClassifyAdaptive(est)", color="#CC6677")
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_xlabel("Margin quartile (Q1=smallest)")
    ax.set_ylabel("False SafeOut rate (%)")
    ax.set_title("False SafeOut Rate by Margin Quartile (True NN only)")
    ax.legend(fontsize=8)
    ax.grid(True, alpha=0.3, axis="y")


def plot_overlap_curve(df, ax):
    """Plot 6: Top-K overlap curve."""
    grouped = df.groupby("probed_count")["topk_overlap"]
    steps = sorted(df["probed_count"].unique())
    mean_overlap = [grouped.get_group(s).mean() for s in steps]
    max_k = df["topk_overlap"].max()

    ax.plot(steps, mean_overlap, "g-o", markersize=4, linewidth=2)
    ax.axhline(y=max_k, color="gray", linestyle="--", alpha=0.5,
               label=f"max possible = {max_k}")
    ax.set_xlabel("Clusters probed")
    ax.set_ylabel("Mean top-K overlap")
    ax.set_title("Top-K Set Overlap: Exact vs RaBitQ Heaps")
    ax.legend(fontsize=9)
    ax.grid(True, alpha=0.3)
    ax.set_ylim(bottom=0)


def main():
    parser = argparse.ArgumentParser(description="Plot RaBitQ diagnostic results")
    parser.add_argument("--dir", required=True, help="Directory containing CSV files")
    parser.add_argument("--save", action="store_true", help="Save PNGs instead of showing")
    args = parser.parse_args()

    d = args.dir

    # Check which CSVs exist
    has_kth = os.path.exists(os.path.join(d, "kth_convergence.csv"))
    has_scatter = os.path.exists(os.path.join(d, "vector_distances.csv"))
    has_classify = os.path.exists(os.path.join(d, "classification.csv"))

    if not has_kth and not has_scatter and not has_classify:
        print(f"No CSV files found in {d}")
        sys.exit(1)

    # ---- Phase 1 plots ----
    if has_kth or has_scatter:
        fig1, axes1 = plt.subplots(2, 2, figsize=(14, 10))
        fig1.suptitle("Phase 1: Distance Distribution Diagnosis", fontsize=14)

        if has_scatter:
            df_scatter = pd.read_csv(os.path.join(d, "vector_distances.csv"))
            plot_scatter(df_scatter, axes1[0, 0])
        else:
            axes1[0, 0].text(0.5, 0.5, "No scatter data", ha="center", va="center",
                             transform=axes1[0, 0].transAxes)

        if has_kth:
            df_kth = pd.read_csv(os.path.join(d, "kth_convergence.csv"))
            plot_kth_convergence(df_kth, axes1[0, 1])
            plot_dmin_dmax(df_kth, axes1[1, 0])
            plot_overlap_curve(df_kth, axes1[1, 1])
        else:
            for ax in [axes1[0, 1], axes1[1, 0], axes1[1, 1]]:
                ax.text(0.5, 0.5, "No kth data", ha="center", va="center",
                        transform=ax.transAxes)

        fig1.tight_layout()
        if args.save:
            fig1.savefig(os.path.join(d, "phase1_distance.png"), dpi=150)
            print(f"Saved {os.path.join(d, 'phase1_distance.png')}")
        else:
            plt.show()

    # ---- Phase 2 plots ----
    if has_classify:
        df_cls = pd.read_csv(os.path.join(d, "classification.csv"))

        fig2, axes2 = plt.subplots(1, 3, figsize=(18, 5))
        fig2.suptitle("Phase 2: ConANN Classification Under RaBitQ", fontsize=14)

        plot_confusion(df_cls, axes2[:2])
        plot_false_safeout_by_margin(df_cls, axes2[2])

        fig2.tight_layout()
        if args.save:
            fig2.savefig(os.path.join(d, "phase2_classification.png"), dpi=150)
            print(f"Saved {os.path.join(d, 'phase2_classification.png')}")
        else:
            plt.show()


if __name__ == "__main__":
    main()
