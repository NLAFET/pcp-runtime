#include <stdlib.h>

#include "../../runtime/pcp.h"
#include "driver.h"
#include "tasks.h"
#include "utils.h"


void parallel_forward_substitution(
    int n,
    int m,
    int blksz,
    double *L,
    int ldL,
    double *X,
    int ldX)
{
    // Register solve task type.
    struct pcp_task_type solve_type =
    {
        .sequential_impl      = solve_task_seq,
        .parallel_impl        = solve_task_par,
        .parallel_reconfigure = solve_task_par_reconfigure,
        .parallel_finalize    = solve_task_par_finalize
    };
    pcp_task_type_handle_t solve_type_handle =
        pcp_register_task_type(&solve_type);

    // Register update task type.
    struct pcp_task_type update_type =
    {
        .sequential_impl = update_task_seq,
        .parallel_impl = update_task_par,
        .parallel_reconfigure = update_task_par_reconfigure,
        .parallel_finalize = update_task_par_finalize
    };
    pcp_task_type_handle_t update_type_handle =
        pcp_register_task_type(&update_type);

    // Start building graph.
    pcp_begin_graph();

    // Compute the number of blocks.
    int N = iceil(n, blksz);

    // Create one lock for each block in B.
    pthread_mutex_t *Blocks = (pthread_mutex_t*) malloc(sizeof(*Blocks) * N);
    for (int i = 0; i < N; ++i) {
        pthread_mutex_init(&Blocks[i], NULL);
    }
    
    // Allocate array of task handles for creating dependencies.
    pcp_task_handle_t *handle_matrix =
        (pcp_task_handle_t*) malloc(sizeof(*handle_matrix) * N * N);
#define HANDLE(i,j) handle_matrix[(i) + (j) * N]
    for (int j = 0; j < N; ++j) {
        for (int i = 0; i < N; ++i) {
            HANDLE(i,j) = PCP_TASK_HANDLE_NULL;
        }
    }

    // Loop through block columns.
    for (int j = 0; j < N; ++j) {
        // Compute the number of columns in this block.
        int ncols = min(n - j * blksz, blksz);

        // Locate tile L(j,j).
        double *Ljj = L + j * blksz * ldL + j * blksz;

        // Locate block X(j).
        double *Xj = X + j * blksz;

        // Insert solve task for L(j,j).
        {
            struct solve_task_arg *arg =
                (struct solve_task_arg*) malloc(sizeof(*arg));
            arg->n    = ncols;
            arg->nrhs = m;
            arg->L    = Ljj;
            arg->ldL  = ldL;
            arg->X    = Xj;
            arg->ldX  = ldX;
            pcp_task_handle_t task = pcp_insert_task(solve_type_handle, arg);

            // Insert dependence on tasks for L(j,0..j-1).
            for (int k = 0; k < j; ++k) {
                pcp_insert_dependence(HANDLE(j,k), task);
            }

            // Record task for L(j, j).
            HANDLE(j,j) = task;
        }
            
        // Loop through block rows below diagonal.
        for (int i = j + 1; i < N; ++i) {
            // Compute the actual number of rows.
            int nrows = min(n - i * blksz, blksz);

            // Locate tile L(i,j).
            double *Lij = L + j * blksz * ldL + i * blksz;

            // Locate block X(i).
            double *Xi = X + i * blksz;
            
            // Insert update task for L(i,j).
            {
                struct update_task_arg *arg =
                    (struct update_task_arg*) malloc(sizeof(*arg));
                arg->nrows = nrows;
                arg->ncols = ncols;
                arg->nrhs  = m;
                arg->L     = Lij;
                arg->ldL   = ldL;
                arg->X     = Xj;
                arg->ldX   = ldX;
                arg->B     = Xi;
                arg->ldB   = ldX;
                arg->Block = &Blocks[i];
                pcp_task_handle_t task = pcp_insert_task(update_type_handle, arg);

                // Insert dependence on task for L(j,j).
                pcp_insert_dependence(HANDLE(j,j), task);

                // Record task for L(i,j).
                HANDLE(i,j) = task;
            }
        }
    }

    // End building the graph.
    pcp_end_graph();

    // Execute the graph.
    pcp_execute_graph();
    
    // Clean up.
    free(handle_matrix);
#undef HANDLE
    for (int i = 0; i < N; ++i) {
        pthread_mutex_destroy(&Blocks[i]);
    }
    free(Blocks);
}
