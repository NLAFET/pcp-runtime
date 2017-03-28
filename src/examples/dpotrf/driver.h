#ifndef EXAMPLES_DPOTRF_DRIVER_H
#define EXAMPLES_DPOTRF_DRIVER_H

/// Computes the Cholesky factorisation of A, A = L * L^T.
/// 
/// @param[in] n The size of the matrix.
/// 
/// @param[in] blksz The tile size.
/// 
/// @param[in,out] A On entry, a symmetric positive definite matrix. On exit
/// the lower triangular matrix L.
/// 
/// @param[in] ldA The column stride of A.
void parallel_block_chol(int n, int blksz, double *A, int ldA);

#endif
