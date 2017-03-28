#include <cblas.h>

#include "tasks.h"


void syrk_task_seq(void *ptr)
{
    struct syrk_task_arg *arg = (struct syrk_task_arg*) ptr;

    int n       = arg->n;
    int k       = arg->k;
    double *A21 = arg->A21;
    double *A22 = arg->A22;
    int ldA     = arg->ldA;

    // Compute A22 := A21 - A21 * A21'.
    cblas_dsyrk(CblasColMajor, CblasLower, CblasNoTrans,
                n, k,
                -1.0, A21, ldA,
                 1.0, A22, ldA);
}
