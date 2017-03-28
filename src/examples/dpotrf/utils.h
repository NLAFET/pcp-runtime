#ifndef EXAMPLES_DPOTRF_UTILS_H
#define EXAMPLES_DPOTRF_UTILS_H


static inline int min(int a, int b)
{
    return a < b ? a : b;
}


static inline int max(int a, int b)
{
    return a > b ? a : b;
}


static inline int iceil(int a, int b)
{
    return (a + b - 1) / b;
}


static inline int ifloor(int a, int b)
{
    return a / b;
}


#endif
