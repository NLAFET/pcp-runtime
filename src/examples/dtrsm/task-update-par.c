#include <pthread.h>
#include <stdlib.h>
#include <cblas.h>

#include "tasks.h"
#include "utils.h"


void update_task_par_reconfigure(int nth)
{
    // empty
}


void update_task_par_finalize(void)
{
    // empty
}


void update_task_par(void *ptr, int nth, int me)
{
    struct update_task_arg *arg = (struct update_task_arg*) ptr;

    int nrows = arg->nrows;
    int ncols = arg->ncols;
    int nrhs  = arg->nrhs;
    double *L = arg->L;
    int ldL   = arg->ldL;
    double *X = arg->X;
    int ldX   = arg->ldX;
    double *B = arg->B;
    int ldB   = arg->ldB;

    // Compute nominal block size.
    int blksz = iceil(nrhs, nth);

    // Determine my share of the right-hand sides.
    int my_first_rhs, my_nrhs;
    my_first_rhs = blksz * me;
    my_nrhs = min(blksz, nrhs - my_first_rhs);

    // Allocate temporary storage W.
    int ldW = nrows;
    double *W = (double*) malloc(sizeof(*W) * ldW * my_nrhs);

    // Compute W := L * X.
    cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans,
                nrows, my_nrhs, ncols,
                1.0, L, ldL,
                     X + my_first_rhs * ldX, ldX,
                0.0, W, ldW);

    // Acquire lock.
    pthread_mutex_lock(arg->Block);

    // Update B := B - W.
#define B(i,j) B[(i) + (j) * ldB]
#define W(i,j) W[(i) + (j) * ldW]
    for (int j = 0; j < my_nrhs; ++j) {
        for (int i = 0; i < nrows; ++i) {
            B(i, my_first_rhs + j) -= W(i,j);
        }
    }
#undef B
#undef W

    // Release lock.
    pthread_mutex_unlock(arg->Block);

    // Free temporary storage.
    free(W);
}
