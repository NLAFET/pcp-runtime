#include <stdlib.h>
#include <cblas.h>

#include "tasks.h"
#include "utils.h"


void solve_task_par_reconfigure(int nth)
{
    // empty
}


void solve_task_par_finalize(void)
{
    // empty
}


void solve_task_par(void *ptr, int nth, int me)
{
    struct solve_task_arg *arg = (struct solve_task_arg*) ptr;

    int n     = arg->n;
    int nrhs  = arg->nrhs;
    double *L = arg->L;
    int ldL   = arg->ldL;
    double *X = arg->X;
    int ldX   = arg->ldX;

    // Compute nominal block size.
    int blksz = iceil(nrhs, nth);

    // Determine my share of the right-hand sides.
    int my_first_rhs, my_nrhs;
    my_first_rhs = blksz * me;
    my_nrhs = min(blksz, nrhs - my_first_rhs);

    // Solve L * X = B, where B and X share the same memory.
    cblas_dtrsm(CblasColMajor, CblasLeft, CblasLower, CblasNoTrans, CblasNonUnit,
                n, my_nrhs,
                1.0, L, ldL,
                     X + my_first_rhs * ldX, ldX);
}
