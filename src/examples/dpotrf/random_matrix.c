#include <stdio.h>
#include <stdlib.h>

#include "random_matrix.h"


static double random_double(const double min, const double max)
{
    const double range = max - min;
    return min + rand() / (RAND_MAX / range);

}

#define A(i,j) A[(i) + (j) * n]

void generate_dense_spd_matrix(const int n, double *const A)
{
    // Generate N-by-N matrix with random values in (0.0, 1.0).
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


void generate_banded_spd_matrix(const int n, const int bandwidth, double *const A)
{
    // Generate banded matrix with random values in (0.0, 1.0).
    for (int j = 0; j < n; j++) {
        for (int i = 0; i < n; i++) {
            if (j < i - bandwidth - 1|| j > i + bandwidth - 1) {
                A(i,j) = 0.0;
            }
            else {
                A(i,j) = random_double(0.0, 1.0);
            }
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


void generate_arrowhead_spd_matrix(const int n, const int bandwidth, const int arrowwidth, double *const A)
{
    // Generate arrowhead matrix with random values in (0.0, 1.0).
    for (int j = 0; j < n - bandwidth; j++) {
        for (int i = 0; i < n - bandwidth ; i++) {
            if (j < i - bandwidth - 1 || j > i + bandwidth - 1 ) {
                A(i,j) = 0.0;
            }
            else {
                A(i,j) = random_double(0.0, 1.0);
            }
        }
    }
    // Initialize right part of arrowhead (left part is set in symmetry step).
    for (int i = 0; i < n; i++) {
        for (int j = n - arrowwidth; j < n; j++) {
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
