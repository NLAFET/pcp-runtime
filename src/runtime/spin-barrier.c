#include <stdlib.h>
#include <stdbool.h>

#include "spin-barrier.h"


struct spin_barrier
{
    int count;
    volatile int arrived;
    volatile int generation;
};


spin_barrier_t *spin_barrier_create(int nth)
{
    spin_barrier_t *bar = (spin_barrier_t*) malloc(sizeof(spin_barrier_t));
    bar->count = nth;
    bar->arrived = 0;
    bar->generation = 0;
    return bar;
}


void spin_barrier_destroy(spin_barrier_t *bar)
{
    free(bar);
}


void spin_barrier_wait(spin_barrier_t *bar)
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


void spin_barrier_wait_and_destroy(spin_barrier_t *bar)
{
    if (__sync_add_and_fetch(&bar->arrived, 1) == bar->count) {
        spin_barrier_destroy(bar);
    }
}
