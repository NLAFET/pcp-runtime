#ifndef EXAMPLES_DPOTRF_RANDOM_MATRIX_H
#define EXAMPLES_DPOTRF_RANDOM_MATRIX_H

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
void generate_dense_spd_matrix(int n, double *A, int ldA);


#endif // EXAMPLES_DPOTRF_RANDOM_MATRIX_H
