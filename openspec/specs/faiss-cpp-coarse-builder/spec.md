# Spec: Faiss C++ Coarse Builder

## Overview

In-process Faiss-based coarse builder for IVF that trains centroids and generates primary assignments without requiring precomputed external artifacts.

## Requirements

### Requirement: Faiss C++ builder can train single coarse partitions in-process
The system SHALL allow `IvfBuilder` to execute `faiss_kmeans` as an in-process CPU coarse builder that trains IVF centroids and generates single nearest-centroid assignments without requiring precomputed `.fvecs/.ivecs` inputs.

#### Scenario: Build runs with faiss_kmeans and no precomputed artifacts
- **WHEN** a build is started with `coarse_builder=faiss_kmeans` and no `centroids_path` or `assignments_path`
- **THEN** the build system SHALL train coarse centroids inside the C++ process using Faiss CPU clustering
- **AND** it SHALL assign every base vector to its nearest centroid to produce primary assignments
- **AND** it SHALL continue through the existing downstream build pipeline without requiring external Python-generated clustering artifacts

### Requirement: Builder identity and clustering source are separate dimensions
The system SHALL represent coarse builder identity independently from clustering source so that `faiss_kmeans` can be used with either in-process training or imported precomputed artifacts.

#### Scenario: Faiss builder is used with precomputed artifacts
- **WHEN** a build is started with `coarse_builder=faiss_kmeans` and valid precomputed centroids and assignments paths
- **THEN** the system SHALL load the provided artifacts instead of retraining
- **AND** it SHALL record `coarse_builder=faiss_kmeans`
- **AND** it SHALL record `clustering_source=precomputed`

#### Scenario: Faiss builder is used with auto training
- **WHEN** a build is started with `coarse_builder=faiss_kmeans` and no precomputed clustering inputs
- **THEN** the system SHALL record `coarse_builder=faiss_kmeans`
- **AND** it SHALL record `clustering_source=auto`

### Requirement: Faiss builder preserves current cosine training semantics
The system SHALL preserve the repository's current coarse-clustering metric semantics when Faiss is used, including cosine normalization behavior and the effective Faiss metric used for training and assignment.

#### Scenario: Cosine metric is requested for faiss_kmeans
- **WHEN** a build is started with cosine-style coarse clustering semantics and `coarse_builder=faiss_kmeans`
- **THEN** the system SHALL normalize vectors consistently with the current Faiss exporter path before clustering
- **AND** it SHALL use the corresponding effective Faiss metric for centroid training and assignment
- **AND** it SHALL expose the effective metric in build metadata

## Builder Identity

| Builder | Identity | Clustering Source |
|---------|----------|------------------|
| Hierarchical SuperKMeans | `hierarchical_superkmeans` | auto |
| Faiss KMeans (in-process) | `faiss_kmeans` | auto |
| Faiss KMeans (precomputed) | `faiss_kmeans` | precomputed |
