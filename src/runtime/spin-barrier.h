#ifndef UTIL_SPIN_BARRIER_H
#define UTIL_SPIN_BARRIER_H

// TODO Add pcp_ prefix.

typedef struct spin_barrier spin_barrier_t;

spin_barrier_t *spin_barrier_create(int nth);
void spin_barrier_destroy(spin_barrier_t *bar);
void spin_barrier_wait(spin_barrier_t *bar);
void spin_barrier_wait_and_destroy(spin_barrier_t *bar);

#endif
