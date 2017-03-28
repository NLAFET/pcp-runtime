#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <mm_malloc.h>

#include "../../runtime/pcp.h"
#include "driver.h"
#include "tasks.h"
#include "utils.h"


void parallel_block_chol(int n, int blksz, double *A, int ldA)
{
    // Register chol task type.
    struct pcp_task_type chol_type =
    {
        .sequential_impl      = chol_task_seq,
        .parallel_impl        = chol_task_par,
        .parallel_reconfigure = chol_task_par_reconfigure,
        .parallel_finalize    = chol_task_par_finalize
    };
    pcp_task_type_handle_t chol_handle = pcp_register_task_type(&chol_type);

    // Register trsm task type.
    struct pcp_task_type trsm_type =
    {
        .sequential_impl      = trsm_task_seq,
        .parallel_impl        = trsm_task_par,
        .parallel_reconfigure = trsm_task_par_reconfigure,
        .parallel_finalize    = trsm_task_par_finalize
    };
    pcp_task_type_handle_t trsm_handle = pcp_register_task_type(&trsm_type);

    // Register syrk task type.
    struct pcp_task_type syrk_type =
    {
        .sequential_impl      = syrk_task_seq,
        .parallel_impl        = syrk_task_par,
        .parallel_reconfigure = syrk_task_par_reconfigure,
        .parallel_finalize    = syrk_task_par_finalize
    };
    pcp_task_type_handle_t syrk_handle = pcp_register_task_type(&syrk_type);

    // Register gemm task type.
    struct pcp_task_type gemm_type =
    {
        .sequential_impl      = gemm_task_seq,
        .parallel_impl        = gemm_task_par,
        .parallel_reconfigure = gemm_task_par_reconfigure,
        .parallel_finalize    = gemm_task_par_finalize
    };
    pcp_task_type_handle_t gemm_handle = pcp_register_task_type(&gemm_type);

    // Start building graph.
    pcp_begin_graph();

    // Compute the number of blocks.
    int num_blks = iceil(n, blksz);

    // Allocate history of handles (a maximum of two iterations have dependences).
    pcp_task_handle_t *handle_matrix = (pcp_task_handle_t*) malloc(sizeof(*handle_matrix) * num_blks * num_blks);
#define HANDLE(i,j) handle_matrix[(i) + (j) * num_blks]
    for (int j = 0; j < num_blks; ++j) {
        for (int i = 0; i < num_blks; ++i) {
            HANDLE(i,j) = PCP_TASK_HANDLE_NULL;
        }
    }

    for (int iter = 0; iter < num_blks; iter++) {
        // Tasks for panel [A11; A21].
        {
            int j = iter;
            double *Ajj = A + j * blksz * ldA + j * blksz;

            // Insert CHOL task for tile A(j,j).
            struct chol_task_arg *arg = (struct chol_task_arg*) malloc(sizeof(*arg));
            arg->n   = min(blksz, n - j * blksz);
            arg->A   = Ajj;
            arg->ldA = ldA;
            pcp_task_handle_t task = pcp_insert_task(chol_handle, arg);

            // Insert dependences.
            if (iter > 0) {
                pcp_insert_dependence(HANDLE(j,j), task);
            }

            // Record CHOL task.
            HANDLE(j,j) = task;

            // Insert TRSM task for tile A(i,j).
            for (int i = j + 1; i < num_blks; i++) {
                struct trsm_task_arg *arg = (struct trsm_task_arg*) malloc(sizeof(*arg));
                arg->m   = min(blksz, n - i * blksz);
                arg->n   = min(blksz, n - iter * blksz);
                arg->A21 = A + j * blksz * ldA + i * blksz; 
                arg->A11 = Ajj;
                arg->ldA = ldA;
                pcp_task_handle_t task = pcp_insert_task(trsm_handle, arg);

                // Insert dependences.
                // (1) CHOL must have been executed before TRSM is started.
                pcp_insert_dependence(HANDLE(j,j), task);
                if (iter > 0) {
                    // (2) The update of my block at the previous iteration must have been completed.
                    pcp_insert_dependence(HANDLE(i,j), task);
                }

                // Record TRSM task.
                HANDLE(i,j) = task;
           }
        }
        
        // Tasks for updating A22.
        for (int i = iter + 1; i < num_blks; i++) {
            for (int j = iter + 1; j <= i; j++) {
                if (i == j) {
                    // Insert SYRK task for tile A(j,j).
                    struct syrk_task_arg *arg = (struct syrk_task_arg*) malloc(sizeof(*arg));
                    arg->n   = min(blksz, n - j * blksz);
                    arg->k   = min(blksz, n - iter * blksz);
                    arg->A21 = A + iter * blksz * ldA + i * blksz;
                    arg->A22 = A + j * blksz * ldA + j * blksz;
                    arg->ldA = ldA;
                    pcp_task_handle_t task = pcp_insert_task(syrk_handle, arg);

                    // Insert dependences.
                    // (1) Use result of TRSM to update me.
                    pcp_insert_dependence(HANDLE(j,iter), task);
                    if (iter > 0) {
                        // (2) The update of my block at the previous iteration must have been completed.
                        pcp_insert_dependence(HANDLE(j,j), task);
                    }

                    // Record SYRK task.
                    HANDLE(j,j) = task;
                }
                else {
                    // Insert GEMM task for tile A(i,j) where i != j.
                    struct gemm_task_arg *arg = (struct gemm_task_arg*) malloc(sizeof(*arg));
                    arg->m    = min(blksz, n - i * blksz);
                    arg->n    = min(blksz, n - j * blksz);
                    arg->k    = min(blksz, n - iter * blksz);
                    arg->A21  = A + iter * blksz * ldA + i * blksz;
                    arg->A21T = A + iter * blksz * ldA + j * blksz;
                    arg->A22  = A + j * blksz * ldA + i * blksz;
                    arg->ldA  = ldA;
                    pcp_task_handle_t task = pcp_insert_task(gemm_handle, arg);

                    // Insert dependences.
                    // (1) A21 must have been updated using TRSM.
                    pcp_insert_dependence(HANDLE(i,iter), task);
                    // (2) A21T must have been updated using TRSM.
                    pcp_insert_dependence(HANDLE(j,iter), task);
                    if (iter > 0) {
                        // (3) The update of my block at the previous iteration must have been completed.
                        pcp_insert_dependence(HANDLE(i,j), task);
                    }

                    // Record GEMM task.
                    HANDLE(i,j) = task;
                }
            }
        }
    } 

    // End building the graph.
    pcp_end_graph();

    // Execute the graph.
    printf("Executing the graph...\n");
    pcp_execute_graph();

    // Clean up.
    free(handle_matrix);
}

