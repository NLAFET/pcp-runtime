#include <stdlib.h>
#include <cblas.h>

#include "tasks.h"



void syrk_task_par_reconfigure(int nth)
{
    // empty
}


void syrk_task_par_finalize(void)
{
    // empty
}


void syrk_task_par(void *ptr, int nth, int me)
{
    struct syrk_task_arg *arg = (struct syrk_task_arg*) ptr;

    int n = arg->n;
    int k = arg->k;
    double *A21 = arg->A21;
    double *A22 = arg->A22;
    int ldA = arg->ldA;

    // Balance the load by flops.
    int part[nth + 1];
    const int total_work = n * (n + 1) / 2;
    const int ideal_part_work = total_work / nth;
    part[0] = 0;
    part[nth] = n;
    for (int k = 1; k < nth; ++k) {
        part[k] = part[k - 1];
        int work = 0;
        while (work < ideal_part_work && part[k] < n) {
            work += n - part[k];
            part[k] += 1;
        }
    }

    const int my_first_col = part[me];
    const int my_num_cols = part[me + 1] - part[me];
    const int i1 = my_first_col;
    const int i2 = i1 + my_num_cols;
    const int m2 = n - i2;

    if (my_num_cols > 0) {
        cblas_dsyrk(CblasColMajor, CblasLower, CblasNoTrans,
                    my_num_cols, k,
                    -1.0, A21 + i1,                           ldA,
                     1.0, A22 + i1 + my_first_col * ldA,      ldA);
    }

    if (m2 > 0) {
        cblas_dgemm(CblasColMajor, CblasNoTrans, CblasTrans,
                    m2, my_num_cols, k,
                    -1.0, A21 + i2,                      ldA,
                          A21 + my_first_col,            ldA,
                     1.0, A22 + i2 + my_first_col * ldA, ldA);
    }
}

