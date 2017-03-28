#include <lapacke.h>

#include "tasks.h"


void chol_task_seq(void *ptr)
{
    struct chol_task_arg *arg = (struct chol_task_arg*) ptr;

    int n     = arg->n;
    double *A = arg->A;
    int ldA   = arg->ldA;

    LAPACKE_dpotrf(LAPACK_COL_MAJOR, 'L', n, A, ldA);
}



