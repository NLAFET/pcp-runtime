#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <cblas.h>

#include "../../runtime/pcp.h"
#include "driver.h"


static double random_double(void)
{
    return (double) rand() / RAND_MAX;
}


static void usage(char *prog)
{
    printf("Usage: %s n m b p q\n", prog);
    printf("\n");
    printf("n: The size of the triangular matrix\n");
    printf("m: The number of right-hand sides\n");
    printf("b: The tile size\n");
    printf("p: The number of threads (or 0 for one per core)\n");
    printf("q: The size of the reserved set (or 0 to use the regular mode)\n");
    exit(EXIT_FAILURE);
}


int main(int argc, char *argv[])
{
    // Verify the number of command line options.
    if (argc != 6 && argc != 7) {
        usage(argv[0]);
    }

    // Parse command line.
    int n = atoi(argv[1]);
    int m = atoi(argv[2]);
    int blksz = atoi(argv[3]);
    int num_threads = atoi(argv[4]);
    int reserved_set_size = atoi(argv[5]);
    int verify = 1;
    if (argc == 7) {
        verify = 0;
    }

    // Verify options.
    if (n < 1 ||
        m < 1 ||
        blksz < 1 ||
        num_threads < 0 ||
        reserved_set_size < 0 ||
        (reserved_set_size >= num_threads && num_threads > 0)) {
        usage(argv[0]);
    }
        
    // Start the runtime system.
    pcp_start(num_threads);

    // Set the execution mode.
    if (reserved_set_size == 0) {
        pcp_set_mode(PCP_REGULAR);
    } else {
        pcp_set_mode(PCP_FIXED);
        pcp_set_num_critical_workers(reserved_set_size);
    }

    // Allocate matrices.
    int ldL, ldX, ldB;
    double *L, *X, *B;
    ldL = ldX = ldB = n;
    L = (double*) malloc(sizeof(double) * ldL * n);
    X = (double*) malloc(sizeof(double) * ldX * m);
    B = (double*) malloc(sizeof(double) * ldB * m);
    
#define L(i,j) L[(i) + (j) * ldL]
#define X(i,j) X[(i) + (j) * ldX]
#define B(i,j) B[(i) + (j) * ldB]

    // Generate L.
    printf("Generating lower triangular matrix L...\n");
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < j; ++i) {
            L(i,j) = 0;
        }
        for (int i = j; i < n; ++i) {
            L(i,j) = random_double();
        }
        L(j,j) = n;
    }

    // Generate B.
    printf("Generating right-hand sides B...\n");
    for (int j = 0; j < m; ++j) {
        for (int i = 0; i < n; ++i) {
            B(i,j) = random_double();
        }
    }

    // Copy B to X.
    printf("Copying B to X...\n");
    for (int j = 0; j < m; ++j) {
        for (int i = 0; i < n; ++i) {
            X(i,j) = B(i,j);
        }
    }

    // Call the driver.
    printf("Calling the driver routine...\n");
    parallel_forward_substitution(n, m, blksz, L, ldL, X, ldX);

    if (verify) {
        // Verify the solution.
        printf("Verifying the solution... ");
        fflush(stdout);
        cblas_dtrmm(CblasColMajor, CblasLeft, CblasLower, CblasNoTrans, CblasNonUnit,
                    n, m,
                    1.0, L, ldL,
                    X, ldX);
        double err = 0;
        for (int j = 0; j < m; ++j) {
            for (int i = 0; i < n; ++i) {
                double e = X(i,j) - B(i,j);
                err += e * e;
            }
        }
        err = sqrt(err);
        if (err / n < 1e-14) {
            printf("PASSED\n");
        } else {
            printf("FAILED\n");
        }
    } else {
        printf("Verification disabled\n");
    }

    // Save trace.
    pcp_view_trace_tikz();

    // Print statistics.
    pcp_view_statistics_stdout();

    // Stop the runtime system.
    pcp_stop();

#undef L
#undef X
#undef B

    // Clean up.
    free(L);
    free(X);
    free(B);
    
    return EXIT_SUCCESS;
}
