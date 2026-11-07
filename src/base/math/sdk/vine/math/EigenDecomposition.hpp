#pragma once

#include "math_global.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "Math.hpp"
#include "Matrix3x3.hpp"
#include "Types.hpp"

VN_MATH_NS_BEGIN

/**
 * @brief Eigen-decomposition of a real symmetric 3×3 matrix by cyclic Jacobi sweeps.
 *
 * @details
 * - Eigenvalues are returned in ascending order λ0 ≤ λ1 ≤ λ2; column i of @p eigenvectors corresponds to λi.
 * - Only the upper triangle of @p m is read, so the caller need not mirror its off-diagonal entries.
 * - Each eigenvector is canonicalized to have a positive largest-magnitude component, so the same input always
 *   produces the same basis; the sign of an eigenvector is otherwise arbitrary.
 * - Handedness of the basis is not constrained: a caller that needs a rotation matrix must rebuild one axis from
 *   the cross product of the other two.
 * - Repeated eigenvalues make the basis non-unique: any orthonormal basis of that eigenspace is a valid answer.
 *
 * @warning
 * - The sweep budget is fixed; a false return means the off-diagonal entries did not reach the tolerance, and the
 *   returned basis should be treated as approximate.
 *
 * @note
 * - It sits in the root namespace rather than in `numeric` because the accuracy target is **not** the caller's: the
 *   budget is fixed and the relative tolerance is `4 * EPS<T>()`. The split is "who sets the target", not "does it
 *   iterate" - a process with a caller-supplied `tolerance` / `max_iterations` that may answer `std::nullopt` lives in
 *   `numeric` (ADR-2026-11-06-shared-numeric-kernels-in-math).
 *
 * @param m            Symmetric matrix.
 * @param eigenvalues  Ascending spectrum.
 * @param eigenvectors Optional orthonormal eigenvector columns; may be null when only the spectrum is needed.
 * @return True when the off-diagonal entries converged within the relative tolerance.
 */
template <FP T, typename Order>
bool symmetricEigen(const Matrix3x3<T, Order>& m, std::array<T, 3>& eigenvalues, Matrix3x3<T>* eigenvectors = nullptr)
{
    constexpr int kMaxSweeps = 16;
    constexpr int kSize      = 3;

    T a[kSize][kSize];
    for (int i = 0; i < kSize; ++i) {
        for (int j = i; j < kSize; ++j) {
            a[i][j] = a[j][i] = m(i, j);
        }
    }

    // Columns of v accumulate the rotations, so they end up as the eigenvectors.
    T v[kSize][kSize] = { { T(1), T(0), T(0) }, { T(0), T(1), T(0) }, { T(0), T(0), T(1) } };

    const T tolerance = EPS<T>() * T(4);
    bool    converged = false;
    for (int sweep = 0; sweep < kMaxSweeps; ++sweep) {
        const T off_diagonal = std::sqrt(a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2]);
        const T scale = std::abs(a[0][0]) + std::abs(a[1][1]) + std::abs(a[2][2]);
        if (off_diagonal <= tolerance * std::max(T(1), scale)) {
            converged = true;
            break;
        }

        for (int p = 0; p < kSize - 1; ++p) {
            for (int q = p + 1; q < kSize; ++q) {
                if (a[p][q] == T(0)) {
                    continue;
                }

                // Rotation that zeroes a[p][q]: the smaller root is chosen for stability.
                const T theta = (a[q][q] - a[p][p]) / (T(2) * a[p][q]);
                const T t     = (theta >= T(0) ? T(1) : T(-1)) / (std::abs(theta) + std::sqrt(theta * theta + T(1)));
                const T c     = T(1) / std::sqrt(t * t + T(1));
                const T s     = t * c;

                for (int k = 0; k < kSize; ++k) {
                    const T akp = a[k][p];
                    const T akq = a[k][q];
                    a[k][p]     = c * akp - s * akq;
                    a[k][q]     = s * akp + c * akq;
                }
                for (int k = 0; k < kSize; ++k) {
                    const T apk = a[p][k];
                    const T aqk = a[q][k];
                    a[p][k]     = c * apk - s * aqk;
                    a[q][k]     = s * apk + c * aqk;
                }
                for (int k = 0; k < kSize; ++k) {
                    const T vkp = v[k][p];
                    const T vkq = v[k][q];
                    v[k][p]     = c * vkp - s * vkq;
                    v[k][q]     = s * vkp + c * vkq;
                }
            }
        }
    }

    // Ascending order: a plain selection sort over three entries keeps the pairing with v explicit.
    std::array<int, 3> order{ 0, 1, 2 };
    for (int i = 0; i < kSize; ++i) {
        for (int j = i + 1; j < kSize; ++j) {
            if (a[order[j]][order[j]] < a[order[i]][order[i]]) {
                std::swap(order[i], order[j]);
            }
        }
    }

    for (int i = 0; i < kSize; ++i) {
        eigenvalues[i] = a[order[i]][order[i]];
    }

    if (eigenvectors != nullptr) {
        for (int i = 0; i < kSize; ++i) {
            const int column = order[i];

            int major = 0;
            for (int k = 1; k < kSize; ++k) {
                if (std::abs(v[k][column]) > std::abs(v[major][column])) {
                    major = k;
                }
            }
            const T sign = v[major][column] < T(0) ? T(-1) : T(1);

            for (int k = 0; k < kSize; ++k) {
                (*eigenvectors)(k, i) = sign * v[k][column];
            }
        }
    }

    return converged;
}

VN_MATH_NS_END
