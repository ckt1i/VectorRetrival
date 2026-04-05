#!/usr/bin/env python3
"""
Faiss GPU KMeans clustering for vector datasets.

Usage:
  conda activate lab
  python scripts/faiss_kmeans.py \
    --input /home/zcq/VDB/data/coco_100k/image_embeddings.npy \
    --nlist 2048 \
    --niter 20 \
    --seed 42 \
    --output-dir /home/zcq/VDB/data/coco_100k

Output:
  {output-dir}/{name}_centroid_{nlist}.fvecs
  {output-dir}/{name}_cluster_id_{nlist}.ivecs
"""

import argparse
import struct
import time
import os

import numpy as np
import faiss
from tqdm import tqdm


def main():
    parser = argparse.ArgumentParser(description="Faiss GPU KMeans")
    parser.add_argument("--input", required=True, help="Path to .npy embeddings")
    parser.add_argument("--nlist", type=int, default=2048, help="Number of clusters")
    parser.add_argument("--niter", type=int, default=20, help="KMeans iterations")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--output-dir", default=None, help="Output directory (default: same as input)")
    parser.add_argument("--name", default=None, help="Output file prefix (default: from input filename)")
    parser.add_argument("--gpu", action="store_true", default=True, help="Use GPU (default)")
    parser.add_argument("--no-gpu", action="store_true", help="Force CPU")
    args = parser.parse_args()

    # Load data
    print(f"Loading {args.input} ...")
    emb = np.load(args.input).astype(np.float32)
    N, dim = emb.shape
    K = args.nlist
    print(f"  N={N}, dim={dim}, K={K}")

    norms = np.linalg.norm(emb, axis=1)
    print(f"  Vector norms: min={norms.min():.4f} mean={norms.mean():.4f} max={norms.max():.4f}")

    # KMeans
    use_gpu = args.gpu and not args.no_gpu and faiss.get_num_gpus() > 0
    print(f"\nRunning KMeans (niter={args.niter}, gpu={use_gpu}) ...")
    t0 = time.time()
    kmeans = faiss.Kmeans(dim, K, niter=args.niter, verbose=True,
                          gpu=use_gpu, seed=args.seed)
    kmeans.train(emb)
    elapsed = time.time() - t0
    print(f"\nKMeans done in {elapsed:.1f}s")

    centroids = kmeans.centroids
    c_norms = np.linalg.norm(centroids, axis=1)
    print(f"  Centroid norms: min={c_norms.min():.4f} mean={c_norms.mean():.4f} max={c_norms.max():.4f}")

    # Assign
    print("\nAssigning vectors to clusters ...")
    _, labels = kmeans.index.search(emb, 1)
    labels = labels.ravel().astype(np.int32)

    residuals = np.linalg.norm(emb - centroids[labels], axis=1)
    print(f"  Residual norms: min={residuals.min():.4f} mean={residuals.mean():.4f} max={residuals.max():.4f}")

    sizes = np.bincount(labels, minlength=K)
    print(f"  Cluster sizes: min={sizes.min()} mean={sizes.mean():.1f} max={sizes.max()} empty={np.sum(sizes==0)}")

    # Output paths
    out_dir = args.output_dir or os.path.dirname(args.input)
    if args.name:
        prefix = args.name
    else:
        prefix = os.path.splitext(os.path.basename(args.input))[0]
        # Simplify: "image_embeddings" -> "coco" etc.
        for strip in ["_embeddings", "_base", "_vectors"]:
            prefix = prefix.replace(strip, "")

    cpath = os.path.join(out_dir, f"{prefix}_centroid_{K}.fvecs")
    apath = os.path.join(out_dir, f"{prefix}_cluster_id_{K}.ivecs")

    # Export .fvecs
    print(f"\nExporting centroids -> {cpath}")
    with open(cpath, 'wb') as f:
        for k in tqdm(range(K), desc="centroids"):
            f.write(struct.pack('i', dim))
            f.write(centroids[k].tobytes())

    # Export .ivecs
    print(f"Exporting assignments -> {apath}")
    with open(apath, 'wb') as f:
        for i in tqdm(range(N), desc="assignments"):
            f.write(struct.pack('i', 1))
            f.write(struct.pack('i', int(labels[i])))

    print("\nDone!")


if __name__ == "__main__":
    main()
