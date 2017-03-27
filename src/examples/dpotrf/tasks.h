#ifndef EXAMPLES_CHOLESKY_TASKS_H
#define EXAMPLES_CHOLESKY_TASKS_H

#include <pthread.h>

static int min(int a, int b)
{
    return a < b ? a : b;
}


// Partitioning:
// [ A00 |  *   *  ]
// [-----+---------]
// [ A10 | A11  *  ]
// [ A20 | A21 A22 ]

// Cholesky factorisation in block A11.
// A11 = chol(A11).
struct chol_task_arg
{
    pthread_mutex_t *A11subblock;
    int n;                      // tile size
    double *A;
    double *workspace;   // b x b matrix
    double *window;      // BLKSZ x BLKSZ matrix (workspace)
    int ldA;
};

void chol_task_seq(void *ptr);
void chol_task_par(void *ptr, int nth, int me);
void chol_task_par_reconfigure(int nth);
void chol_task_par_finalize(void);

void naive_chol_task_seq(void *ptr);
void naive_chol_task_par(void *ptr, int nth, int me);
void naive_chol_task_par_reconfigure(int nth);
void naive_chol_task_par_finalize(void);

void naive_chol_task_par_2(void *ptr, int nth, int me);
void naive_chol_task_par_2_reconfigure(int nth);
void naive_chol_task_par_2_finalize(void);


// Update panel A21 in blocks.
// A21 = A21 * A11^-T.
struct trsm_task_arg
{
    int m;
    int n;
    double *A21;    // m x n matrix
    double *A11;    // n x n matrix
    int ldA;
};

void trsm_task_seq(void *ptr);
void trsm_task_par(void *ptr, int nth, int me);
void trsm_task_par_reconfigure(int nth);
void trsm_task_par_finalize(void);


// Update A22 in blocks.
// A22 = A22 - A21 * A21^T
// As we do the update in blocks, we have a case distinction
// A22(i,j) = A22(i,j) - A21(i,:) * A21(j,:)^T.
// Case 1: i == j. Use SYRK.
// Case 2: i != j. A21(i,:) and A21(j,:)^T do not overlap. Use GEMM.
//
struct gemm_task_arg
{
    int m;
    int n;
    int k;
    double *A21;    // m x k matrix
    double *A21T;   // n x k matrix
    double *A22;    // m x n matrix
    int ldA;
};

void gemm_task_seq(void *ptr);
void gemm_task_par(void *ptr, int nth, int me);
void gemm_task_par_reconfigure(int nth);
void gemm_task_par_finalize(void);


// A22 = A22 - A21 * A21^T
struct syrk_task_arg
{
    int n;
    int k;
    double *A21;    // n x k matrix
    double *A22;    // n x n matrix
    int ldA;
};

void syrk_task_seq(void *ptr);
void syrk_task_par(void *ptr, int nth, int me);
void syrk_task_par_reconfigure(int nth);
void syrk_task_par_finalize(void);


#endif // EXAMPLES_CHOLESKY_TASKS_H
