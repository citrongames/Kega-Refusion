#ifndef KEGA32X_CLOCK_H
#define KEGA32X_CLOCK_H
#include <stdint.h>

/* SH-2 runs at 3/7 of the master clock. Keep absolute timestamps exact,
   including when ticks are too large to multiply by three in 64 bits. */
static inline uint64_t sh2_clock(uint64_t ticks)
{
#if defined(__SIZEOF_INT128__)
    /* 7 * scale = 3 * 2^64 + 1. The rounding term in ticks * scale
       contributes less than 1/7, so it cannot change floor(3*ticks/7). */
    const __uint128_t scale = (((__uint128_t)3 << 64) + 1) / 7;
    return (uint64_t)((ticks * scale) >> 64);
#else
    return (ticks / 7) * 3 + ((ticks % 7) * 3) / 7;
#endif
}
#endif
