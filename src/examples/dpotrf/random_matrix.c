#include <stdio.h>
#include <stdlib.h>

#include "random_matrix.h"


static double random_double(double min, double max)
{
    double range = max - min;
    return min + rand() / (RAND_MAX / range);
}

#define A(i,j) A[(i) + (j) * n]

void generate_dense_spd_matrix(int n, double *A, int ldA)
{
    // Generate n-by-n matrix with random values in (0.0, 1.0).
    for (int j = 0; j < n; j++) {
        for (int i = 0; i < n; i++) {
            A(i,j) = random_double(0.0, 1.0);
        }
    }
    
    // Ensure symmetry with A = 0.5 * (A + A^T).
    for (int j = 0; j < n; j++) {
        for (int i = 0; i < n; i++) {
            A(i,j) = 0.5 * ( A(i,j) + A(j,i) );
            A(i,j) = A(j,i);
        }
    }
    
    // Make A diagonally dominant with A = A + n * I.
    for (int i = 0; i < n; i++) {
        A(i,i) += n;
    }
}

