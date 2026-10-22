#include "spsc_ring.h"

#include <string.h>

/*
 * Ordering rule: write the slot, THEN publish the index (release); read the
 * index (acquire), THEN touch the slot. If the other side sees the new index,
 * it is guaranteed to see the slot contents too.
 *
 * The UART data register itself is already volatile (vendor header); the
 * bytes copied out of it are plain RAM, so this ring relies on atomics, not
 * volatile.
 */

static uint8_t *slot(const spsc_ring_t *r, uint32_t index)
{
    return r->buf + (size_t)(index & r->mask) * r->elem_size;
}

bool spsc_init(spsc_ring_t *r, void *storage, uint32_t elem_size, uint32_t capacity)
{
    if (r == NULL || storage == NULL || elem_size == 0u || capacity == 0u ||
        (capacity & (capacity - 1u)) != 0u) {
        return false;
    }
    r->buf = storage;
    r->elem_size = elem_size;
    r->mask = capacity - 1u;
    atomic_init(&r->head, 0u);
    atomic_init(&r->tail, 0u);
    return true;
}

bool spsc_push(spsc_ring_t *r, const void *elem)
{
    uint32_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);

    if (head - tail > r->mask) {
        return false; /* full */
    }
    memcpy(slot(r, head), elem, r->elem_size);
    atomic_store_explicit(&r->head, head + 1u, memory_order_release);
    return true;
}

bool spsc_pop(spsc_ring_t *r, void *out)
{
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&r->head, memory_order_acquire);

    if (head == tail) {
        return false; /* empty */
    }
    memcpy(out, slot(r, tail), r->elem_size);
    atomic_store_explicit(&r->tail, tail + 1u, memory_order_release);
    return true;
}

uint32_t spsc_peek(spsc_ring_t *r, void *out, uint32_t max)
{
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    uint32_t n = head - tail;

    if (n > max) {
        n = max;
    }
    for (uint32_t i = 0; i < n; i++) {
        memcpy((uint8_t *)out + (size_t)i * r->elem_size, slot(r, tail + i), r->elem_size);
    }
    return n;
}

void spsc_release(spsc_ring_t *r, uint32_t n)
{
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&r->head, memory_order_acquire);

    if (n > head - tail) {
        n = head - tail;
    }
    atomic_store_explicit(&r->tail, tail + n, memory_order_release);
}

uint32_t spsc_count(spsc_ring_t *r)
{
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    uint32_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    return head - tail;
}
