#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <mm_malloc.h>

#include "../../runtime/pcp.h"
#include "driver.h"
#include "tasks.h"


void parallel_block_chol(int n, int blksz, double *const A, double *workspace, double *window)
{
    // Register chol task type (Compute Cholesky factorisation in a submatrix).
#if 0
    // Optimized implementation.
    struct pcp_task_type chol_type =
    {
        .sequential_impl = chol_task_seq,
        .parallel_impl = chol_task_par,
        .parallel_reconfigure = chol_task_par_reconfigure,
        .parallel_finalize = chol_task_par_finalize
    };
#endif
#if 1
    // Naive implementation with calls to LAPACK and BLAS.
    struct pcp_task_type chol_type =
    {
        .sequential_impl = naive_chol_task_seq,
        .parallel_impl = naive_chol_task_par,
        .parallel_reconfigure = naive_chol_task_par_reconfigure,
        .parallel_finalize = naive_chol_task_par_finalize
    };
#endif
    pcp_task_type_handle_t chol_handle = pcp_register_task_type(&chol_type);

    // Register trsm task type.
    struct pcp_task_type trsm_type =
    {
        .sequential_impl = trsm_task_seq,
        .parallel_impl = trsm_task_par,
        .parallel_reconfigure = trsm_task_par_reconfigure,
        .parallel_finalize = trsm_task_par_finalize
    };
    pcp_task_type_handle_t trsm_handle = pcp_register_task_type(&trsm_type);

    // Register syrk task type.
    struct pcp_task_type syrk_type =
    {
        .sequential_impl = syrk_task_seq,
        .parallel_impl = syrk_task_par,
        .parallel_reconfigure = syrk_task_par_reconfigure,
        .parallel_finalize = syrk_task_par_finalize
    };
    pcp_task_type_handle_t syrk_handle = pcp_register_task_type(&syrk_type);

    // Register gemm task type.
    struct pcp_task_type gemm_type =
    {
        .sequential_impl = gemm_task_seq,
        .parallel_impl = gemm_task_par,
        .parallel_reconfigure = gemm_task_par_reconfigure,
        .parallel_finalize = gemm_task_par_finalize
    };
    pcp_task_type_handle_t gemm_handle = pcp_register_task_type(&gemm_type);


    // Start building graph.
    pcp_begin_graph();

    // Compute the number of blocks.
    const int num_blks = (n + blksz - 1) / blksz;

    // Compute the number of iterations.
    const int max_iters = (n + blksz - 1)/blksz;

    // Keep track of the dependence between two successive iterations.
    const int prev_iter = 0;
    const int cur_iter = 1;

    // Allocate history of handles (a maximum of two iterations have dependences).
    pcp_task_handle_t *handle_matrix = (pcp_task_handle_t*) malloc(sizeof(*handle_matrix) * n * n * 2);
#define HANDLE(i,j,iter) handle_matrix[(i) + (j) * n + (iter) * n * n]
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            HANDLE(i,j,prev_iter) = PCP_TASK_HANDLE_NULL;
            HANDLE(i,j,cur_iter)  = PCP_TASK_HANDLE_NULL;
        }
    }


    // Allocate lock for parallel chol task.
    pthread_mutex_t A11subblock;
    pthread_mutex_init(&A11subblock, NULL);


    // Right-looking Cholesky factorisation.
    //
    // Partitioning:
    // [ A11 |  *  ]
    // [-----+-----]
    // [ A21 | A22 ]
    //
    // A11 = CHOL(A11)          (CHOL)
    // A21 = A21 * A11^-T       (TRSM)
    // A22 = A22 - A21 * A21^T  (SYRK, GEMM)
    //
    // Reference: https://www.cs.utexas.edu/users/plapack/icpp98/node2.html


    for (int iter = 0; iter < max_iters; iter++) {
        // Tasks for panel [A11; A21].
        {
            int j = iter;
            double *Ajj = A + j * blksz * n + j * blksz;

            // Insert CHOL task for block (j, j).
            struct chol_task_arg *arg = (struct chol_task_arg*) malloc(sizeof(*arg));
            arg->A11subblock = &A11subblock;
            arg->A = Ajj;
            arg->n = min(blksz, n - j * blksz);
            arg->workspace = workspace;
            arg->window = window;
            arg->ldA = n;
            pcp_task_handle_t task = pcp_insert_task(chol_handle, arg);

            // Record CHOL task.
            HANDLE(j,j,cur_iter) = task;

            // Insert dependences. The previous iteration must have been completed.
            if (iter > 0) {
                pcp_insert_dependence(HANDLE(j,j,prev_iter), task);
            }

            // Insert TRSM task for block (i, j).
            for (int i = j+1; i <num_blks; i++) {
                struct trsm_task_arg *arg = (struct trsm_task_arg*) malloc(sizeof(*arg));
                arg->m = min(blksz, n - i * blksz);
                arg->n = min(blksz, n - iter * blksz);
                arg->A21 = A + j * blksz * n + i * blksz; // me
                arg->A11 = Ajj; // CHOL block
                arg->ldA = n;
                pcp_task_handle_t task = pcp_insert_task(trsm_handle, arg);

                // Record TRSM task.
                HANDLE(i,j,cur_iter) = task;

                // Insert two dependences.
                // (1) CHOL must have been executed before TRSM is started.
                pcp_insert_dependence(HANDLE(j,j,cur_iter), task);
                if (iter > 0) {
                    // (2) The update of my block at the previous timestep must have been completed.
                    pcp_insert_dependence(HANDLE(i,j,prev_iter), task);
                }
            }
        }
        // Tasks for updating A22.
        for (int i = iter + 1; i < num_blks; i++) {
            for (int j = iter + 1; j <= i; j++) {
                if (i == j) {
                    // Insert SYRK task for block (j, j).
                    struct syrk_task_arg *arg = (struct syrk_task_arg*) malloc(sizeof(*arg));
                    arg->n = min(blksz, n - j * blksz);
                    arg->k = min(blksz, n - iter * blksz);
                    arg->A21 = A + iter * blksz * n + i * blksz;
                    arg->A22 = A + j * blksz * n + j * blksz;
                    arg->ldA = n;
                    pcp_task_handle_t task = pcp_insert_task(syrk_handle, arg);

                    // Record SYRK task.
                    HANDLE(j,j,cur_iter) = task;

                    // Insert two dependences.
                    // (1) Use result of TRSM to update me.
                    pcp_insert_dependence(HANDLE(j,iter,cur_iter), task);
                    if (iter > 0) {
                        // (2) The update of my block at the previous timestep must have been completed.
                        pcp_insert_dependence(HANDLE(j,j,prev_iter), task);
                    }
                }
                else {
                    // Insert GEMM task for block (i, j) where i != j.
                    struct gemm_task_arg *arg = (struct gemm_task_arg*) malloc(sizeof(*arg));
                    arg->m = min(blksz, n - i * blksz);
                    arg->n = min(blksz, n - j * blksz);
                    arg->k = min(blksz, n - iter * blksz);
                    arg->A21 = A + iter * blksz * n + i * blksz;
                    arg->A21T = A + iter * blksz * n + j * blksz;
                    arg->A22 = A + j * blksz * n + i * blksz; // me
                    arg->ldA = n;
                    pcp_task_handle_t task = pcp_insert_task(gemm_handle, arg);

                    // Record GEMM task.
                    HANDLE(i,j,cur_iter) = task;

                    // Insert three dependences.
                    // (1) A21 must have been updated using TRSM.
                    pcp_insert_dependence(HANDLE(i,iter,cur_iter), task);
                    // (2) A21T must have been updated using TRSM.
                    pcp_insert_dependence(HANDLE(j,iter,cur_iter), task);
                    if (iter > 0) {
                        // (3) The update of my block at the previous timestep must have been completed.
                        pcp_insert_dependence(HANDLE(i,j,prev_iter), task);
                    }
                }
            }
        }

        // Overwrite old iteration with this iteration.
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                HANDLE(i,j,prev_iter) = HANDLE(i,j,cur_iter);
                HANDLE(i,j,cur_iter)  = PCP_TASK_HANDLE_NULL;
            }
        }

    } // for iter

    // End building the graph.
    pcp_end_graph();

    // Execute the graph.
    printf("Execute the task graph...\n");
    pcp_execute_graph();

    // Clean up.
    pthread_mutex_destroy(&A11subblock);
    free(handle_matrix);
}

