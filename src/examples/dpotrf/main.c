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

// TODO Remove.
#define BLKSZ 480

// TODO Remove.
#define blocksize 16

// Types of spd matrices.
typedef enum { DENSE, BANDED, ARROWHEAD } spd_matrix_type; 

// Align data structures to 32-byte memory addresses.
#define ALIGNMENT 32


static double gettime(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + 1e-6 * tv.tv_usec;
}


static void compare_lower_triangular_matrices(int n, double *L1, double *L2);


int main(int argc, char** argv)
{
    int verify = 1;
    
    if (argc != 5 && argc != 6) {
        printf("Usage: %s matrix-size blksz num-workers num-critical-workers\n", argv[0]);
        printf("\n");
        printf("matrix-size:          The size of the triangular matrix\n");
        printf("blksz:                The tile size\n");
        printf("num-workers:          The number of workers (threads) or 0 for one thread per core\n");
        printf("num-critical-workers: The fixed number of critical workers or -1 for adaptive or -2 for prescribed\n");
        return EXIT_FAILURE;
    }

    if (argc == 6) {
        verify = 0;
    }
    
    // Initialise random number generator.
    srand(time(NULL));

    // Parse input parameters.
    int n = atoi(argv[1]);
    int blksz = atoi(argv[2]);
    int num_workers = atoi(argv[3]);
    int num_critical_workers = atoi(argv[4]);

    /* if (n % blksz != 0) { */
    /*     printf("Please choose the dimensions of the system as a multiple of the block size\n"); */
    /*     return EXIT_FAILURE; */
    /* } */

    /* if (blksz % 32 != 0) { */
    /*     printf("Please choose the block size as a multiple of 32\n"); */
    /*     return EXIT_FAILURE; */
    /* } */

    printf("Matrix size is set to %d, the block size is %d.\n", n, blksz);

    // Set A to a symmetric positive definite matrix.
    spd_matrix_type matrix_type = DENSE; // DENSE, BANDED, ARROWHEAD     
    double *A = (double *) _mm_malloc(n*n*sizeof(double), ALIGNMENT);     
    switch (matrix_type) {     
    case DENSE:     
    {
        printf("Generate dense symmetric positive definite matrix A...\n");         
        generate_dense_spd_matrix(n, A);     
    }
    break;

    case BANDED:
    {         
        const int bandwidth = 2*blksz;         
        printf("Generate banded symmetric positive definite matrix A with bandwidth = %d...\n", bandwidth);         
        generate_banded_spd_matrix(n, bandwidth, A);     
    }     
    break;

    case ARROWHEAD:     
    {
        // Compute the number of blocks.
        const int num_blks = (n + blksz - 1) / blksz;

        // The band and the arrowhead fill blocks completely.
        const int num_blks_band = 1;
        const int bandwidth = num_blks_band * blksz;

        // The arrowhead fills the same number of blocks as the band.
        const int arrowwidth = num_blks_band * blksz - (num_blks * blksz - n);

        printf("Generate arrowhead symmetric positive definite matrix A with bandwidth = %d and arrowwidth = %d...\n", bandwidth, arrowwidth);
        generate_arrowhead_spd_matrix(n, bandwidth, arrowwidth, A);
    }     
    break;
    }

    // Copy A to Ain.
    printf("Copy A to preserve a copy of the input matrix...\n");
    double *Ain = (double *) _mm_malloc(n*n*sizeof(double), ALIGNMENT);
    memcpy(Ain, A, n*n*sizeof(double));

    // Allocate workspace.
    printf("Allocate workspace...\n");
    double *workspace = (double *) _mm_malloc(num_workers * blocksize * blocksize * sizeof(double), ALIGNMENT);
    double *window = (double *) _mm_malloc(blksz * blksz * sizeof(double), ALIGNMENT);

    // Start the runtime system.
    pcp_start(num_workers);
    if (num_critical_workers >= 0) {
        pcp_set_mode(PCP_FIXED);
        pcp_set_num_critical_workers(num_critical_workers);
    }
    if (num_critical_workers == -1) {
        pcp_set_mode(PCP_ADAPTIVE);
    }
    if (num_critical_workers == -2) {
        pcp_set_mode(PCP_PRESCRIBED);
    }

    // Call the driver.
    printf("Call the driver routine...\n");
    parallel_block_chol(n, blksz, A, workspace, window);

    if (verify) {
        // Verify the solution.
        printf("Solve using LAPACKE dpotrf...\n");
        double tm_dpotrf = gettime();
        LAPACKE_dpotrf(LAPACK_COL_MAJOR, 'L', n, Ain, n);
        tm_dpotrf = gettime() - tm_dpotrf;
        printf("DPOTRF execution time = %.6lf\n", tm_dpotrf);
        printf("Verify the computed solution...\n");
        compare_lower_triangular_matrices(n, Ain, A);
    }

    // Save the trace.
    pcp_view_trace_tikz();

    // View statistics.
    pcp_view_statistics_stdout();

    // Stop the runtime system.
    pcp_stop();

    // Clean up.
    _mm_free(A);
    _mm_free(Ain);
    _mm_free(workspace);
    _mm_free(window);

    return EXIT_SUCCESS;
}


static void compare_lower_triangular_matrices(
    int n,
    double *restrict const L1, double *restrict const L2)
{
#define L1(i,j) L1[(i) + (j) * n]
#define L2(i,j) L2[(i) + (j) * n]

    // Write zeros explicitly
    #pragma omp simd aligned(L1:ALIGNMENT), aligned(L2:ALIGNMENT)
    for (int i = 0; i < n; i++) {
        for (int j = i+1; j < n; j++) {
            L1(i,j) = 0.0;
            L2(i,j) = 0.0;
        }
    }

    // validate ||L1 - L2|| == 0
    #pragma omp simd aligned(L1:ALIGNMENT), aligned(L2:ALIGNMENT)
    for (int j = 0; j < n; j++) {
        for (int i = 0; i < n; i++) {
            L1(i,j) = L1(i,j) - L2(i,j);
        }
    }

    printf("1-norm         = %.6le\n", LAPACKE_dlange(LAPACK_COL_MAJOR, '1', n, n, L1, n));
    printf("Frobenius-norm = %.6le\n", LAPACKE_dlange(LAPACK_COL_MAJOR, 'F', n, n, L1, n));

#undef L1
#undef L2
}
