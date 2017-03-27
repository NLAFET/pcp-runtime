#include <stdlib.h>
#include <cblas.h>

#include "tasks.h"


void gemm_task_par_reconfigure(int nth)
{
    // empty
}


void gemm_task_par_finalize(void)
{
    // empty
}

void gemm_task_par(void *ptr, int nth, int me)
{
    struct gemm_task_arg *arg = (struct gemm_task_arg*) ptr;

    int m = arg->m;
    int n = arg->n;
    int k = arg->k;
    double *A21 = arg->A21;
    double *A21T = arg->A21T;
    double *A22 = arg->A22;
    int ldA = arg->ldA;

    // Cut A21T into panels of size m-by-blksz. Compute nominal block size.
    const int blksz = (n + nth -1) / nth;

    // Determine my share of A21T.
    const int my_first_col = blksz * me;
    const int my_num_cols  = min(blksz, n - my_first_col);

    // Compute A22(i,j) = A22(i,j) - A21(i,:) * A21(j,:)^T.
    cblas_dgemm(CblasColMajor, CblasNoTrans, CblasTrans,
                m, my_num_cols, k,
                -1.0, A21,                      ldA,
                      A21T + my_first_col,      ldA,
                 1.0, A22 + my_first_col * ldA, ldA);
}

