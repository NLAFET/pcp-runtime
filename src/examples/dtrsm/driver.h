#ifndef EXAMPLES_DTRSM_DRIVER_H
#define EXAMPLES_DTRSM_DRIVER_H


/**
 * Solves a lower triangular linear system
 *
 *     L * X = B
 *
 * of dimension n with m right-hand sides.
 *
 * The solution X overwrites B.
 *
 * @param n The number of equations.
 *
 * @param m The number of right-hand sides.
 *
 * @param blksz The tile size.
 *
 * @param L The lower triangular coefficient matrix.
 *
 * @param ldL The column stride of L.
 *
 * @param X On entry, the matrix of right-hand sides. On exit, the
 * solution matrix.
 *
 * @param ldX The column stride of X.
 */
void parallel_forward_substitution(int n, int m, int blksz, double *L, int ldL, double *X, int ldX);


#endif
