#include <cblas.h>

#include "tasks.h"


void solve_task_seq(void *ptr)
{
    struct solve_task_arg *arg = (struct solve_task_arg*) ptr;

    int n     = arg->n;
    int nrhs  = arg->nrhs;
    double *L = arg->L;
    int ldL   = arg->ldL;
    double *X = arg->X;
    int ldX   = arg->ldX;

    // Solve L * X = B, where B and X share the same memory.
    cblas_dtrsm(CblasColMajor, CblasLeft, CblasLower, CblasNoTrans, CblasNonUnit,
                n, nrhs,
                1.0, L, ldL,
                     X, ldX);
}
