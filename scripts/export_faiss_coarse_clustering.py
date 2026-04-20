#!/home/zcq/anaconda3/envs/labnew/bin/python
from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

import faiss
import numpy as np


def write_fvecs(path: Path, arr: np.ndarray) -> None:
    arr = np.asarray(arr, dtype=np.float32)
    with path.open("wb") as handle:
        dim = arr.shape[1]
        for row in arr:
            handle.write(struct.pack("<I", dim))
            handle.write(row.tobytes(order="C"))


def write_ivecs(path: Path, arr: np.ndarray) -> None:
    arr = np.asarray(arr, dtype=np.int32).reshape(-1, 1)
    with path.open("wb") as handle:
        for row in arr:
            handle.write(struct.pack("<I", 1))
            handle.write(row.astype(np.int32).tobytes(order="C"))


def load_base_matrix(path: Path) -> np.ndarray:
    if path.suffix != ".npy":
        raise ValueError(f"unsupported base format for Faiss exporter: {path}")
    return np.load(path).astype(np.float32)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", required=True)
    parser.add_argument("--nlist", type=int, required=True)
    parser.add_argument("--metric", choices=["l2", "ip", "cosine"], default="cosine")
    parser.add_argument("--train-size", type=int, default=100000)
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()

    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    base = load_base_matrix(Path(args.base).resolve())
    if args.metric == "cosine":
        norms = np.linalg.norm(base, axis=1, keepdims=True)
        norms[norms == 0] = 1.0
        base = base / norms
        faiss_metric = faiss.METRIC_INNER_PRODUCT
        effective_metric = "ip"
    elif args.metric == "ip":
        faiss_metric = faiss.METRIC_INNER_PRODUCT
        effective_metric = "ip"
    else:
        faiss_metric = faiss.METRIC_L2
        effective_metric = "l2"

    dim = int(base.shape[1])
    index = faiss.index_factory(dim, f"IVF{args.nlist},Flat", faiss_metric)
    train_size = min(args.train_size, int(base.shape[0]))
    index.train(base[:train_size])
    centroids = index.quantizer.reconstruct_n(0, index.nlist)
    _, assignments = index.quantizer.search(base, 1)

    centroids_path = output_dir / "centroids.fvecs"
    assignments_path = output_dir / "assignments.ivecs"
    write_fvecs(centroids_path, centroids)
    write_ivecs(assignments_path, assignments.reshape(-1))

    (output_dir / "builder_meta.json").write_text(
        json.dumps(
            {
                "coarse_builder": "faiss_kmeans",
                "metric": args.metric,
                "effective_metric": effective_metric,
                "nlist": args.nlist,
                "rows": int(base.shape[0]),
                "dim": dim,
                "train_size": train_size,
                "centroids_path": str(centroids_path),
                "assignments_path": str(assignments_path),
            },
            indent=2,
            ensure_ascii=False,
        )
        + "\n"
    )


if __name__ == "__main__":
    main()
