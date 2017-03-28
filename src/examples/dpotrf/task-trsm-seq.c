#include <cblas.h>

#include "tasks.h"

void trsm_task_seq(void *ptr)
{
    struct trsm_task_arg *arg = (struct trsm_task_arg*) ptr;

    int n       = arg->n;
    int m       = arg->m;
    double *A21 = arg->A21;
    double *A11 = arg->A11;
    int ldA     = arg->ldA;

    // Compute A21 := A21 * inv(A11').
    cblas_dtrsm(CblasColMajor, CblasRight, CblasLower, CblasTrans, CblasNonUnit,
                m, n,
                1.0, A11, ldA,
                     A21, ldA);
}

