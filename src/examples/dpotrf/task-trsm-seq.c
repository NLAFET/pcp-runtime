#include <stdlib.h>
#include <cblas.h>

#include "tasks.h"

void trsm_task_seq(void *ptr)
{
    struct trsm_task_arg *arg = (struct trsm_task_arg*) ptr;

    int n = arg->n;
    int m = arg->m;
    double *A21 = arg->A21; // m x n matrix
    double *A11 = arg->A11; // n x n matrix
    int ldA = arg->ldA;

    // Solve A21 = A21 * A11^-T.
    cblas_dtrsm(CblasColMajor, CblasRight, CblasLower, CblasTrans, CblasNonUnit,
                m, n,
                1.0, A11, ldA,
                     A21, ldA);
}

