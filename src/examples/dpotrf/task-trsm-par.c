#include <stdlib.h>
#include <cblas.h>

#include "tasks.h"


void trsm_task_par_reconfigure(int nth)
{
    // empty
}


void trsm_task_par_finalize(void)
{
    // empty
}


void trsm_task_par(void *ptr, int nth, int me)
{
    struct trsm_task_arg *arg = (struct trsm_task_arg*) ptr;

    int n       = arg->n;
    int m       = arg->m;
    double *A21 = arg->A21; // m x n matrix
    double *A11 = arg->A11; // n x n matrix
    int ldA     = arg->ldA;

    // Compute nominal block size.
    int blksz = (m + nth - 1) / nth;

    // Determine my share of the rows of A21.
    const int my_first_row = blksz * me;
    const int my_num_rows = min(blksz, m - my_first_row);

    // Solve A21 = A21 * A11^-T.
    if (my_num_rows > 0) {
        cblas_dtrsm(CblasColMajor, CblasRight, CblasLower, CblasTrans, CblasNonUnit,
                    my_num_rows, n,
                    1.0, A11,                ldA,
                         A21 + my_first_row, ldA);
    }

}
