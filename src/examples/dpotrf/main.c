#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <math.h>
#include <sys/time.h>
#include <mm_malloc.h>
#include <cblas.h>
#include <lapacke.h>

#include "random_matrix.h"
#include "driver.h"
#include "../../runtime/pcp.h"


static double compare_lower_triangular_matrices(int n, double *L1, int ldL1, double *L2, int ldL2)
{
#define L1(i,j) L1[(i) + (j) * ldL1]
#define L2(i,j) L2[(i) + (j) * ldL2]

    // Clear out the upper triangular part.
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < j; i++) {
            L1(i,j) = 0.0;
            L2(i,j) = 0.0;
        }
    }

    // Compute L1 := L1 - L2.
    for (int j = 0; j < n; j++) {
        for (int i = 0; i < n; i++) {
            L1(i,j) = L1(i,j) - L2(i,j);
        }
    }
             
    // Compute || L1 ||_F.
    double error = LAPACKE_dlange(LAPACK_COL_MAJOR, 'F', n, n, L1, ldL1);
    error /= n;
    return error;

#undef L1
#undef L2
}


static void usage(char *prog)
{
    printf("Usage: %s n b p q\n", prog);
    printf("\n");
    printf("n: The size of the matrix\n");
    printf("b: The tile size\n");
    printf("p: The number of threads (or 0 for one per core)\n");
    printf("q: The size of the reserved set (or 0 to use the regular mode)\n");
    exit(EXIT_FAILURE);
}
    

int main(int argc, char** argv)
{
    // Verify the number of command line options.
    if (argc != 5 && argc != 6) {
        usage(argv[0]);
    }

    // Parse command line.
    int n = atoi(argv[1]);
    int blksz = atoi(argv[2]);
    int num_threads = atoi(argv[3]);
    int reserved_set_size = atoi(argv[4]);
    int verify = 1;
    if (argc == 6) {
        verify = 0;
    }

    // Verify options.
    if (n < 1 ||
        blksz < 1 ||
        num_threads < 0 ||
        reserved_set_size < 0 ||
        (reserved_set_size >= num_threads && num_threads > 0)) {
        usage(argv[0]);
    }
    
    // Initialise random number generator.
    srand(time(NULL));

    // Generate random SPD matrix A.
    printf("Generating SPD matrix A...\n");
    int ldA = n;
    double *A = (double *) malloc(n * ldA * sizeof(double));     
    generate_dense_spd_matrix(n, A, ldA);     

    // Copy A to Ain.
    printf("Copying A to Ain...\n");
    double *Ain = (double *) malloc(n * ldA * sizeof(double));
    memcpy(Ain, A, n * ldA * sizeof(double));

    // Start the runtime system.
    pcp_start(num_threads);

    // Set the execution mode.
    if (reserved_set_size == 0) {
        pcp_set_mode(PCP_REGULAR);
    } else {
        pcp_set_mode(PCP_FIXED);
        pcp_set_reserved_set_size(reserved_set_size);
    }

    // Call the driver.
    printf("Calling the driver routine...\n");
    parallel_block_chol(n, blksz, A, ldA);

    if (verify) {
        // Verify the solution.
        printf("Verifying the solution... ");
        fflush(stdout);
        double tm = pcp_get_time();
        LAPACKE_dpotrf(LAPACK_COL_MAJOR, 'L', n, Ain, ldA);
        tm = pcp_get_time() - tm;
        double error = compare_lower_triangular_matrices(n, Ain, ldA, A, ldA);
        if (error < 1e-14) {
            printf("PASSED\n");
        } else {
            printf("FAILED\n");
        }
        printf("LAPACK_dpotrf took %.6lf seconds\n", tm);
    } else {
        printf("Verification disabled\n");
    }

    // Save the trace.
    printf("Saving the trace...\n");
    pcp_view_trace_tikz();

    // View statistics.
    printf("Printing statistics...\n");
    pcp_view_statistics_stdout();

    // Stop the runtime system.
    pcp_stop();

    // Clean up.
    free(A);
    free(Ain);

    return EXIT_SUCCESS;
}

