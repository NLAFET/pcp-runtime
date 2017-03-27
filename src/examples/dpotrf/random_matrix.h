#ifndef EXAMPLES_CHOLESKY_RANDOM_MATRIX_H
#define EXAMPLES_CHOLESKY_RANDOM_MATRIX_H

/// @brief Generate a random dense symmetric positive definite matrix.
///
/// @par Purpose
/// Creates a dense n-by-n matrix A s.t. A is symmetric positive definite.
/// By construction, A is diagonally dominant and exhibits only positive
/// entries.
///
/// @param[in] n   - matrix dimension
/// @param[out] A  - On exit, a n-by-n symmetric positive definite matrix.
///
void generate_dense_spd_matrix(const int n, double *const A);


/// @brief Generate a random banded symmetric positive definite matrix.
///
/// @par Purpose
/// Creates a banded n-by-n matrix A s.t. A is symmetric positive definite.
/// Zero entries are explicitly initialized. By construction, A is diagonally
/// dominant.
///
/// @param[in] n         - matrix dimension
/// @param[in] bandwidth - size of the band, e.g. 2 gives a tridiagonal matrix.
/// @param[out] A        - On exit, a banded symmetric positive definite matrix.
///
void generate_banded_spd_matrix(const int n, const int bandwidth, double *const A);


/// @brief Generate a banded arrowhead symmetric positive definite matrix.
///
/// @par Purpose
/// Creates a symmetric positive definite matrix of the form
///
/// x         x
///   x       x
///     x     x
///       x   x
///         x x
/// x x x x x x
///
/// where x has dimension bandwidth. Zero entries are explicitly initialized.
/// By construction, A is diagonally dominant.
///
/// @param[in] n         - matrix dimension
/// @param[in] bandwidth - size of the band, e.g. 2 gives a tridiagonal matrix.
/// @param[in] arrowwidth- size of the arrowhead.
/// @param[out] A        - On exit, an arrowhead symmetric positive definite matrix.
///
void generate_arrowhead_spd_matrix(const int n, const int bandwidth, const int arrowwidth, double *const A);

#endif // EXAMPLES_CHOLESKY_RANDOM_MATRIX_H
