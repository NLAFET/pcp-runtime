#include <stdlib.h>
#include <cblas.h>

#include "tasks.h"


void gemm_task_seq(void *ptr)
{
    struct gemm_task_arg *arg = (struct gemm_task_arg*) ptr;

    int m = arg->m;
    int n = arg->n;
    int k = arg->k;
    double *A21 = arg->A21;
    double *A21T = arg->A21T;   // Not yet transposed!
    double *A22 = arg->A22;
    int ldA = arg->ldA;

    // Compute A22(i,j) = A22(i,j) - A21(i,:) * A21(j,:)^T.
    cblas_dgemm(CblasColMajor, CblasNoTrans, CblasTrans,
                m, n, k,
                -1.0, A21,  ldA,
                      A21T, ldA,
                 1.0, A22,  ldA);
}
