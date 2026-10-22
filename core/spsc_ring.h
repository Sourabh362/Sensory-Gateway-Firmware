/*
 * spsc_ring.h - lock-free single-producer / single-consumer ring of fixed-size
 * elements. Safe between an ISR and the main loop, or across cores, because
 * head is written only by the producer and tail only by the consumer.
 */
#ifndef SPSC_RING_H
#define SPSC_RING_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t         *buf;
    uint32_t         elem_size;
    uint32_t         mask; /* capacity - 1, capacity is a power of 2 */
    _Atomic uint32_t head; /* written only by the producer */
    _Atomic uint32_t tail; /* written only by the consumer */
} spsc_ring_t;

/* storage must hold capacity * elem_size bytes; capacity must be a power of 2. */
bool spsc_init(spsc_ring_t *r, void *storage, uint32_t elem_size, uint32_t capacity);

/* Producer only. Returns false if full; the caller counts the drop. */
bool spsc_push(spsc_ring_t *r, const void *elem);

/* Consumer only. */
bool     spsc_pop(spsc_ring_t *r, void *out);
uint32_t spsc_peek(spsc_ring_t *r, void *out, uint32_t max); /* copy, don't remove */
void     spsc_release(spsc_ring_t *r, uint32_t n);           /* remove after peek */

/* Either side; a snapshot that may change immediately. */
uint32_t spsc_count(spsc_ring_t *r);

#endif /* SPSC_RING_H */
