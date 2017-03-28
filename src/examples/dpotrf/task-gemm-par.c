#include <cblas.h>

#include "tasks.h"
#include "utils.h"


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

    int m        = arg->m;
    int n        = arg->n;
    int k        = arg->k;
    double *A21  = arg->A21;
    double *A21T = arg->A21T;
    double *A22  = arg->A22;
    int ldA      = arg->ldA;

    // Cut A21T into panels of size m-by-blksz. Compute nominal block size.
    int blksz = iceil(n, nth);

    // Determine my share of A21T.
    int my_first_col = blksz * me;
    int my_num_cols  = min(blksz, n - my_first_col);

    // Compute A22 := A22 - A21 * A21T', using my share of A22 and A21T.
    cblas_dgemm(CblasColMajor, CblasNoTrans, CblasTrans,
                m, my_num_cols, k,
                -1.0, A21,                      ldA,
                      A21T + my_first_col,      ldA,
                 1.0, A22 + my_first_col * ldA, ldA);
}

