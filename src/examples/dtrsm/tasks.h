#ifndef EXAMPLES_TRI_SOLVER_TASKS_H
#define EXAMPLES_TRI_SOLVER_TASKS_H

#include <pthread.h>

struct solve_task_arg
{
    double *L;
    double *X;
    int n;
    int nrhs;
    int ldL;
    int ldX;
};

void solve_task_seq(void *ptr);
void solve_task_par(void *ptr, int nth, int me);
void solve_task_par_reconfigure(int nth);
void solve_task_par_finalize(void);

struct update_task_arg
{
    pthread_mutex_t *Block;
    double *L;
    double *X;
    double *B;
    int nrows;
    int ncols;
    int nrhs;
    int ldL;
    int ldX;
    int ldB;
};

void update_task_seq(void *ptr);
void update_task_par(void *ptr, int nth, int me);
void update_task_par_reconfigure(int nth);
void update_task_par_finalize(void);

#endif
