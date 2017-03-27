#include <stdlib.h>
#include <stdio.h>
#include <cblas.h>
#include <lapacke.h>
#include <sys/time.h>

#include "tasks.h"


void naive_chol_task_seq(void *ptr)
{
    struct chol_task_arg *arg = (struct chol_task_arg*) ptr;

    double *A = arg->A;
    int ldA = arg->ldA;
    int n = arg->n;

    LAPACKE_dpotrf(LAPACK_COL_MAJOR, 'L', n, A, ldA);
}



