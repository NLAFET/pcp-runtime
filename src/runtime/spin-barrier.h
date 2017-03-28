#ifndef RUNTIME_SPIN_BARRIER_H
#define RUNTIME_SPIN_BARRIER_H

typedef struct pcp_barrier pcp_barrier_t;

pcp_barrier_t *pcp_barrier_create(int nth);
void pcp_barrier_destroy(pcp_barrier_t *bar);
void pcp_barrier_wait(pcp_barrier_t *bar);
void pcp_barrier_wait_and_destroy(pcp_barrier_t *bar);

#endif
