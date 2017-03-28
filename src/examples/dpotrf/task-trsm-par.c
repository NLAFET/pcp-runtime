#include <cblas.h>

#include "tasks.h"
#include "utils.h"


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
    double *A21 = arg->A21; 
    double *A11 = arg->A11; 
    int ldA     = arg->ldA;

    // Compute nominal block size.
    int blksz = iceil(m, nth);

    // Determine my share of the rows of A21.
    int my_first_row = blksz * me;
    int my_num_rows  = min(blksz, m - my_first_row);

    // Compute A21 := A21 * inv(A11'), using my block of A21.
    if (my_num_rows > 0) {
        cblas_dtrsm(CblasColMajor, CblasRight, CblasLower, CblasTrans, CblasNonUnit,
                    my_num_rows, n,
                    1.0, A11,                ldA,
                         A21 + my_first_row, ldA);
    }

}
