#include <pthread.h>
#include <stdlib.h>
#include <cblas.h>

#include "tasks.h"


void update_task_seq(void *ptr)
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

    // Allocate temporary storage W.
    int ldW = nrows;
    double *W = (double*) malloc(sizeof(*W) * ldW * nrhs);

    // Compute W := L * X.
    cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans,
                nrows, nrhs, ncols,
                1.0, L, ldL,
                     X, ldX,
                0.0, W, ldW);

    // Acquire lock.
    pthread_mutex_lock(arg->Block);

    // Update B := B - W.
#define B(i,j) B[(i) + (j) * ldB]
#define W(i,j) W[(i) + (j) * ldW]
    for (int j = 0; j < nrhs; ++j) {
        for (int i = 0; i < nrows; ++i) {
            B(i,j) -= W(i,j);
        }
    }
#undef B
#undef W

    // Release lock.
    pthread_mutex_unlock(arg->Block);

    // Free temporary storage.
    free(W);
}
