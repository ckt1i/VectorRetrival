#include "vdb/rabitq/rabitq_rotation.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <numeric>

namespace vdb {
namespace rabitq {

// ============================================================================
// Construction / Move
// ============================================================================

RotationMatrix::RotationMatrix(Dim dim)
    : dim_(dim), data_(static_cast<size_t>(dim) * dim, 0.0f) {}

RotationMatrix::RotationMatrix(Dim dim, std::vector<float> data)
    : dim_(dim), data_(std::move(data)) {}

RotationMatrix::RotationMatrix(RotationMatrix&& other) noexcept
    : dim_(other.dim_),
      data_(std::move(other.data_)),
      use_fast_hadamard_(other.use_fast_hadamard_),
      diag_signs_(std::move(other.diag_signs_)) {
    other.dim_ = 0;
    other.use_fast_hadamard_ = false;
}

RotationMatrix& RotationMatrix::operator=(RotationMatrix&& other) noexcept {
    if (this != &other) {
        dim_ = other.dim_;
        data_ = std::move(other.data_);
        use_fast_hadamard_ = other.use_fast_hadamard_;
        diag_signs_ = std::move(other.diag_signs_);
        other.dim_ = 0;
        other.use_fast_hadamard_ = false;
    }
    return *this;
}

// ============================================================================
// GenerateRandom — QR decomposition of random Gaussian matrix
// ============================================================================

void RotationMatrix::GenerateRandom(uint64_t seed) {
    use_fast_hadamard_ = false;
    diag_signs_.clear();

    const size_t L = dim_;
    const size_t n = L * L;

    // Step 1: Fill with i.i.d. N(0,1) values
    std::mt19937_64 rng;
    if (seed == 0) {
        std::random_device rd;
        rng.seed(rd());
    } else {
        rng.seed(seed);
    }
    std::normal_distribution<float> dist(0.0f, 1.0f);

    data_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        data_[i] = dist(rng);
    }

    // Step 2: Modified Gram-Schmidt QR decomposition (column-oriented)
    // We treat the matrix as column-major for Gram-Schmidt, then
    // the result Q is stored row-major (Q[i][j] = data_[i*L + j]).
    //
    // Actually, we'll work with columns stored contiguously in a temp buffer.
    // Column j of the matrix is data_[0*L+j], data_[1*L+j], ..., data_[(L-1)*L+j]
    //
    // For efficiency, work on rows as columns (transpose interpretation):
    // Treat data_ as L column vectors of length L stored row-major.
    // Row i = column vector i.
    // After Gram-Schmidt on rows, the rows will be orthonormal.

    // Gram-Schmidt on rows: make row i orthogonal to rows 0..i-1
    for (size_t i = 0; i < L; ++i) {
        float* row_i = data_.data() + i * L;

        // Subtract projections onto previous rows
        for (size_t j = 0; j < i; ++j) {
            const float* row_j = data_.data() + j * L;
            // dot(row_i, row_j)
            float dot = 0.0f;
            for (size_t k = 0; k < L; ++k) {
                dot += row_i[k] * row_j[k];
            }
            // row_i -= dot * row_j
            for (size_t k = 0; k < L; ++k) {
                row_i[k] -= dot * row_j[k];
            }
        }

        // Normalize row_i
        float norm = 0.0f;
        for (size_t k = 0; k < L; ++k) {
            norm += row_i[k] * row_i[k];
        }
        norm = std::sqrt(norm);
        if (norm > 1e-10f) {
            float inv_norm = 1.0f / norm;
            for (size_t k = 0; k < L; ++k) {
                row_i[k] *= inv_norm;
            }
        }
    }
}

// ============================================================================
// GenerateHadamard — Walsh-Hadamard × random diagonal
// ============================================================================

namespace {

/// Check if n is a power of 2.
bool IsPowerOf2(uint32_t n) {
    return n > 0 && (n & (n - 1)) == 0;
}

/// In-place Fast Walsh-Hadamard Transform (unnormalized).
/// After this, vec[i] contains the transform coefficient.
/// The transform is its own inverse (up to scaling by 1/n).
void FWHT_InPlace(float* vec, uint32_t n) {
    for (uint32_t len = 1; len < n; len <<= 1) {
        for (uint32_t i = 0; i < n; i += len << 1) {
            for (uint32_t j = 0; j < len; ++j) {
                float u = vec[i + j];
                float v = vec[i + j + len];
                vec[i + j]       = u + v;
                vec[i + j + len] = u - v;
            }
        }
    }
}

}  // namespace

bool RotationMatrix::GenerateHadamard(uint64_t seed, bool use_fast_transform) {
    if (!IsPowerOf2(dim_)) {
        return false;
    }

    const size_t L = dim_;

    // Generate random diagonal signs
    std::mt19937_64 rng;
    if (seed == 0) {
        std::random_device rd;
        rng.seed(rd());
    } else {
        rng.seed(seed);
    }

    diag_signs_.resize(L);
    std::uniform_int_distribution<int> coin(0, 1);
    for (size_t i = 0; i < L; ++i) {
        diag_signs_[i] = coin(rng) ? +1 : -1;
    }

    // Build the full matrix P = (1/√L) H × D
    // H is the Walsh-Hadamard matrix (H[i][j] = (-1)^popcount(i&j))
    // D is the diagonal matrix with diag_signs_
    //
    // P[i][j] = (1/√L) × H[i][j] × D[j][j]
    //         = (1/√L) × (-1)^popcount(i&j) × diag_signs_[j]

    const float scale = 1.0f / std::sqrt(static_cast<float>(L));
    data_.resize(L * L);

    for (size_t i = 0; i < L; ++i) {
        for (size_t j = 0; j < L; ++j) {
            // H[i][j] = (-1)^popcount(i & j)
            int pc = __builtin_popcount(static_cast<uint32_t>(i & j));
            float h_val = (pc & 1) ? -1.0f : 1.0f;
            data_[i * L + j] = scale * h_val * static_cast<float>(diag_signs_[j]);
        }
    }

    use_fast_hadamard_ = use_fast_transform;
    return true;
}

// ============================================================================
// Apply — P^T × in (encoding/query rotation)
// ============================================================================

void RotationMatrix::Apply(const float* VDB_RESTRICT in,
                           float* VDB_RESTRICT out) const {
    const size_t L = dim_;

    if (use_fast_hadamard_) {
        // Fast path: O(L log L)
        // P^T = D^T H^T / √L = D H / √L  (since H is symmetric, D is diagonal)
        // So P^T × in = (1/√L) D (H × in)
        //
        // Step 1: copy in → out
        std::memcpy(out, in, L * sizeof(float));

        // Step 2: Apply FWHT in-place
        FWHT_InPlace(out, static_cast<uint32_t>(L));

        // Step 3: Scale by 1/√L and multiply by D
        const float scale = 1.0f / std::sqrt(static_cast<float>(L));
        for (size_t i = 0; i < L; ++i) {
            out[i] *= scale * static_cast<float>(diag_signs_[i]);
        }
    } else {
        // General path: O(L²) matrix-vector multiply
        // out = P^T × in → out[j] = Σ_i P[i][j] × in[i] = Σ_i data_[i*L+j] × in[i]
        // Equivalent to: out[j] = column j of P dotted with in
        //
        // For better cache behavior, iterate over rows of P^T = columns of P:
        // out[j] = Σ_i P[i][j] * in[i]
        // This is a column-access pattern on P. We can reformulate:
        // out = P^T × in, where P^T[j][i] = P[i][j]
        //
        // We iterate: for each output element j, sum over i.
        // But for better cache usage, accumulate: for each row i of P, scatter.
        std::memset(out, 0, L * sizeof(float));
        for (size_t i = 0; i < L; ++i) {
            const float in_i = in[i];
            const float* row = data_.data() + i * L;
            for (size_t j = 0; j < L; ++j) {
                out[j] += row[j] * in_i;
            }
        }
    }
}

// ============================================================================
// ApplyInverse — P × in (inverse rotation)
// ============================================================================

void RotationMatrix::ApplyInverse(const float* VDB_RESTRICT in,
                                  float* VDB_RESTRICT out) const {
    const size_t L = dim_;

    if (use_fast_hadamard_) {
        // P = (1/√L) H × D
        // P × in = (1/√L) H × (D × in)
        //
        // Step 1: Apply D to in → out
        for (size_t i = 0; i < L; ++i) {
            out[i] = in[i] * static_cast<float>(diag_signs_[i]);
        }

        // Step 2: Apply FWHT in-place
        FWHT_InPlace(out, static_cast<uint32_t>(L));

        // Step 3: Scale by 1/√L
        const float scale = 1.0f / std::sqrt(static_cast<float>(L));
        for (size_t i = 0; i < L; ++i) {
            out[i] *= scale;
        }
    } else {
        // General path: out = P × in → out[i] = Σ_j P[i][j] × in[j]
        // = row i of P dotted with in (row-access: cache-friendly)
        for (size_t i = 0; i < L; ++i) {
            const float* row = data_.data() + i * L;
            float sum = 0.0f;
            for (size_t j = 0; j < L; ++j) {
                sum += row[j] * in[j];
            }
            out[i] = sum;
        }
    }
}

}  // namespace rabitq
}  // namespace vdb
