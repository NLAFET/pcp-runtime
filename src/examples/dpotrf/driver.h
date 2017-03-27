#ifndef EXAMPLES_CHOLESKY_DRIVER_H
#define EXAMPLES_CHOLESKY_DRIVER_H

///
/// Computes the Cholesky factorisation of A, A = L * L^T.
/// @param[in] n The dimension of the system.
/// @param[in] blksz The block size.
/// @param[in,out] A On entry, a symmetric positive definite matrix. On exit
/// the lower triangular matrix L.
void parallel_block_chol(int n, int blksz, double *const A, double *workspace, double *window);

#endif // EXAMPLES_CHOLESKY_DRIVER_H
