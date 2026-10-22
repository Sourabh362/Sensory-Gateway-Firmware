/*
 * Single-writer counters: only the owning context increments, any context
 * reads. Plain atomic load + store (no read-modify-write), so it stays
 * lock-free even on cores without atomic RMW instructions.
 */
#ifndef COUNTER_H
#define COUNTER_H

#include <stdatomic.h>
#include <stdint.h>

typedef _Atomic uint32_t counter_t;

static inline void counter_add(counter_t *c, uint32_t n)
{
    atomic_store_explicit(c, atomic_load_explicit(c, memory_order_relaxed) + n,
                          memory_order_relaxed);
}

static inline void counter_inc(counter_t *c)
{
    counter_add(c, 1u);
}

static inline uint32_t counter_get(counter_t *c)
{
    return atomic_load_explicit(c, memory_order_relaxed);
}

#endif /* COUNTER_H */
