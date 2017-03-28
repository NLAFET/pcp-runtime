#include <stdlib.h>
#include <cblas.h>
#include <lapacke.h>

#include "../../runtime/pcp.h"
#include "tasks.h"
#include "utils.h"


#define BLOCK_SIZE 32
#define MAX_MATRIX_SIZE 1024
#define MAX_NUM_ITER (MAX_MATRIX_SIZE / BLOCK_SIZE)
#define MAX_NUM_THREADS 16


// The matrix partitioning for TRSM and SYRK in each iteration.
//
// A partitioning x of a range 0, ..., n - 1 over p threads is
// represented as an array x[] of length p + 1 such that x[k] is the
// start of the block assigned to thread k and x[k + 1] - x[k] is the
// size of the block assigned to thread k, for k = 0, ..., p - 1. 
struct partition
{
    // The matrix size. 
    int matrix_size;

    // The block size.
    int block_size;

    // The number of threads.
    int num_threads;

    // The number of iterations = ceil(matrix_size, block_size).
    int num_iter;

    // The partitionings for TRSM. 
    int trsm[MAX_NUM_ITER][MAX_NUM_THREADS + 1];

    // The partitionings for SYRK.
    int syrk[MAX_NUM_ITER][MAX_NUM_THREADS + 1];
};


// Barrier used to synchronize the workers between iterations.
static pcp_barrier_t *barrier = NULL;

static struct partition *partition_db[MAX_MATRIX_SIZE + 1][MAX_NUM_THREADS + 1] = {NULL};
static struct partition *partition = NULL;

static double *A;
static int ldA;
static int n;
#define A(i,j) A[(i) + (j) * ldA]



// Initializes the partitioning for TRSM for a given iteration.
//
// Uses uniform row blocks. 
static void part_init_trsm(int iter)
{
    const int last = partition->matrix_size;
    const int first = min(last, (iter + 1) * partition->block_size);
    const int size = last - first;
    const int p = partition->num_threads;
    const int chunk = iceil(size, p);
    partition->trsm[iter][0] = first;
    partition->trsm[iter][p] = last;
    for (int th = 1; th < p; ++th) {
        partition->trsm[iter][th] = min(last, partition->trsm[iter][th - 1] + chunk);
    }
}


// Initializes the partitioning for SYRK for a given iteration.
static void part_init_syrk(int iter)
{
    // Balance the load by flops.
    const int last = partition->matrix_size;
    const int first = min(last, (iter + 1) * partition->block_size);
    const int size = last - first;
    const int p = partition->num_threads;
    const int total_work = size * (size + 1) / 2;
    const int ideal_part_work = total_work / p;
    partition->syrk[iter][0] = first;
    partition->syrk[iter][p] = last;
    for (int k = 1; k < p; ++k) {
        partition->syrk[iter][k] = partition->syrk[iter][k - 1];
        int work = 0;
        while (work < ideal_part_work && partition->syrk[iter][k] < last) {
            partition->syrk[iter][k] += 1;
            work += partition->syrk[iter][k] - first;
        }
        if (k == 1) {
            partition->syrk[iter][k] = max(partition->syrk[iter][k], partition->syrk[iter][k - 1] + partition->block_size);
            partition->syrk[iter][k] = min(partition->syrk[iter][k], last);
        }
    }
}


// Returns the start of a TRSM block. 
static int part_block_start_trsm(int iter, int me)
{
    return partition->trsm[iter][me];
}


// Returns the size of a TRSM block. 
static int part_block_size_trsm(int iter, int me)
{
    return part_block_start_trsm(iter, me + 1) - part_block_start_trsm(iter, me);
}


// Returns the start of a SYRK block. 
static int part_block_start_syrk(int iter, int me)
{
    return partition->syrk[iter][me];
}


// Returns the size of a SYRK block. 
static int part_block_size_syrk(int iter, int me)
{
    return part_block_start_syrk(iter, me + 1) - part_block_start_syrk(iter, me);
}


// Initializes the partitioning for given parameters.
//
// Follows this protocol:
// 1) Re-use existing if it matches.
// 2) Load from file if it matches.
// 3) Create from scratch.
static void part_init(int matrix_size, int block_size, int num_threads)
{
    // Check if the partitioning is available in the databse.
    if (partition_db[matrix_size][num_threads] == NULL) {
        // No, create from scratch and save in the database.
        partition = malloc(sizeof(*partition));
        partition->matrix_size = matrix_size;
        partition->block_size = block_size;
        partition->num_threads = num_threads;
        partition->num_iter = iceil(matrix_size, block_size);
        for (int iter = 0; iter < partition->num_iter; ++iter) {
            part_init_trsm(iter);
            part_init_syrk(iter);
        }
        partition_db[matrix_size][num_threads] = partition;
    } else {
        // Yes, reuse from database.
        partition = partition_db[matrix_size][num_threads];
    }
}


void chol_task_par_reconfigure(int nth)
{
    // Choose current size of the worker pool for barrier.
    if (barrier != NULL) {
        pcp_barrier_destroy(barrier);
    }
    barrier = pcp_barrier_create(nth);
}


void chol_task_par_finalize(void)
{
    if (barrier != NULL) {
        pcp_barrier_destroy(barrier);
        barrier = NULL;
    }
}


static void krnl_chol(int iter)
{
    int jp      = iter * partition->block_size;
    int n       = min(partition->matrix_size - jp, partition->block_size);
    double *A11 = &A(jp,jp);

    LAPACKE_dpotrf(LAPACK_COL_MAJOR, 'L', n, A11, ldA);    
}


static void krnl_syrk(int iter, int me)
{
    //   j1        j2
    // i g g g g g s
    //   g g g g g s s 
    //   g g g g g s s s 
    //   g g g g g s s s s
    //
    // g = gemm
    // s = syrk

    int jp = iter * partition->block_size;
    int i  = part_block_start_syrk(iter, me);
    int m  = part_block_size_syrk(iter, me);
    int j1 = jp + partition->block_size;
    int j2 = i;
    int n1 = j2 - j1;
    int k  = partition->block_size;

    double *A21;
    double *A21a, *A21b;
    double *A22;

    /* SYRK */
    if (m > 0) {
        A22 = &A(i,j2);
        A21 = &A(i,jp);
        cblas_dsyrk(CblasColMajor, CblasLower, CblasNoTrans,
                    m, k, 
                    -1.0, A21, ldA,
                     1.0, A22, ldA);
    }

    /* GEMM */
    if (m > 0 && n1 > 0) {
        A22  = &A(i,j1);
        A21a = &A(i,jp);
        A21b = &A(j1,jp);
        cblas_dgemm(CblasColMajor, CblasNoTrans, CblasTrans,
                    m, n1, k,
                    -1.0, A21a, ldA,
                          A21b, ldA,
                     1.0, A22,  ldA);
    }
}


static void krnl_trsm(int iter, int me)
{
    int jp      = iter * partition->block_size;
    int i       = part_block_start_trsm(iter, me);
    int m       = part_block_size_trsm(iter, me);
    int n       = min(partition->matrix_size - jp, partition->block_size);
    double *A11 = &A(jp,jp);
    double *A21 = &A(i,jp);

    if (m > 0) {
        cblas_dtrsm(CblasColMajor, CblasRight, CblasLower, CblasTrans, CblasNonUnit,
                    m, n,
                    1.0, A11, ldA,
                         A21, ldA);
    }
}


void chol_task_par(void *ptr, int nth, int me)
{
    struct chol_task_arg *arg = (struct chol_task_arg*) ptr;
       
    n   = arg->n;
    A   = arg->A;
    ldA = arg->ldA;

    if (me == 0) {
        part_init(n, BLOCK_SIZE, nth);
    }

    if (me == 0) {
        // chol(0)
        krnl_chol(0);
    }

    // Synchronize.
    pcp_barrier_wait(barrier);

    // Loop over iterations.
    for (int iter = 0; iter < partition->num_iter; ++iter) {
        const int final_iteration = (iter == partition->num_iter - 1);

        // Synchronize
        pcp_barrier_wait(barrier);

        //
        // Phase I: trsm(iter) in //
        //

        krnl_trsm(iter, me);

        // Synchronize
        pcp_barrier_wait(barrier);

        //
        // Phase II: syrk(iter) in // plus chol(iter + 1)
        //

        krnl_syrk(iter, me);
        if (me == 0 && !final_iteration) {
            krnl_chol(iter + 1);
        }
    }

    // Synchronize
    pcp_barrier_wait(barrier);
}
