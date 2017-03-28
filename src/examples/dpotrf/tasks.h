#ifndef EXAMPLES_DPOTRF_TASKS_H
#define EXAMPLES_DPOTRF_TASKS_H


struct chol_task_arg
{
    int n;
    double *A;
    int ldA;
};

void chol_task_seq(void *ptr);
void chol_task_par(void *ptr, int nth, int me);
void chol_task_par_reconfigure(int nth);
void chol_task_par_finalize(void);


struct trsm_task_arg
{
    int m;
    int n;
    double *A21;  
    double *A11;  
    int ldA;
};

void trsm_task_seq(void *ptr);
void trsm_task_par(void *ptr, int nth, int me);
void trsm_task_par_reconfigure(int nth);
void trsm_task_par_finalize(void);


struct gemm_task_arg
{
    int m;
    int n;
    int k;
    double *A21;  
    double *A21T; 
    double *A22;  
    int ldA;
};

void gemm_task_seq(void *ptr);
void gemm_task_par(void *ptr, int nth, int me);
void gemm_task_par_reconfigure(int nth);
void gemm_task_par_finalize(void);


struct syrk_task_arg
{
    int n;
    int k;
    double *A21;  
    double *A22;  
    int ldA;
};

void syrk_task_seq(void *ptr);
void syrk_task_par(void *ptr, int nth, int me);
void syrk_task_par_reconfigure(int nth);
void syrk_task_par_finalize(void);


#endif
