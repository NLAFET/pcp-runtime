#include <stdlib.h>
#include <stdbool.h>

#include "spin-barrier.h"


struct pcp_barrier
{
    int count;
    volatile int arrived;
    volatile int generation;
};


pcp_barrier_t *pcp_barrier_create(int nth)
{
    pcp_barrier_t *bar = (pcp_barrier_t*) malloc(sizeof(pcp_barrier_t));
    bar->count = nth;
    bar->arrived = 0;
    bar->generation = 0;
    return bar;
}


void pcp_barrier_destroy(pcp_barrier_t *bar)
{
    free(bar);
}


void pcp_barrier_wait(pcp_barrier_t *bar)
{
    const int generation = bar->generation;
    if (__sync_add_and_fetch(&bar->arrived, 1) == bar->count) {
        bar->arrived = 0;
        __sync_add_and_fetch(&bar->generation, 1);
    } else {
        while (generation == bar->generation)
            ;
    }
}


void pcp_barrier_wait_and_destroy(pcp_barrier_t *bar)
{
    if (__sync_add_and_fetch(&bar->arrived, 1) == bar->count) {
        pcp_barrier_destroy(bar);
    }
}
