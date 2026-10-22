#include "spsc_ring.h"
#include "test_assert.h"

#include <pthread.h>
#include <stdint.h>

static void test_init_rejects_bad_args(void)
{
    spsc_ring_t r;
    uint8_t storage[16];

    CHECK(!spsc_init(&r, storage, 1, 0));
    CHECK(!spsc_init(&r, storage, 1, 12)); /* not a power of 2 */
    CHECK(!spsc_init(&r, storage, 0, 16));
    CHECK(!spsc_init(&r, NULL, 1, 16));
    CHECK(spsc_init(&r, storage, 1, 16));
}

static void test_push_pop_order(void)
{
    spsc_ring_t r;
    uint8_t storage[8];
    uint8_t v;

    CHECK(spsc_init(&r, storage, 1, 8));
    CHECK(!spsc_pop(&r, &v));
    for (uint8_t i = 0; i < 5; i++) {
        CHECK(spsc_push(&r, &i));
    }
    CHECK_EQ(spsc_count(&r), 5);
    for (uint8_t i = 0; i < 5; i++) {
        CHECK(spsc_pop(&r, &v));
        CHECK_EQ(v, i);
    }
    CHECK(!spsc_pop(&r, &v));
    CHECK_EQ(spsc_count(&r), 0);
}

static void test_full_rejects_push(void)
{
    spsc_ring_t r;
    uint8_t storage[4];
    uint8_t v = 7;

    CHECK(spsc_init(&r, storage, 1, 4));
    for (int i = 0; i < 4; i++) {
        CHECK(spsc_push(&r, &v));
    }
    CHECK(!spsc_push(&r, &v));
    CHECK(spsc_pop(&r, &v));
    CHECK(spsc_push(&r, &v));
}

static void test_wraps_many_times(void)
{
    spsc_ring_t r;
    uint32_t storage[4];
    uint32_t v;

    CHECK(spsc_init(&r, storage, sizeof(uint32_t), 4));
    for (uint32_t i = 0; i < 1000; i++) {
        CHECK(spsc_push(&r, &i));
        CHECK(spsc_pop(&r, &v));
        CHECK_EQ(v, i);
    }
}

static void test_index_overflow_at_uint32_max(void)
{
    spsc_ring_t r;
    uint32_t storage[4];
    uint32_t v;

    CHECK(spsc_init(&r, storage, sizeof(uint32_t), 4));
    atomic_store(&r.head, UINT32_MAX - 1u);
    atomic_store(&r.tail, UINT32_MAX - 1u);

    for (uint32_t i = 0; i < 4; i++) {
        CHECK(spsc_push(&r, &i));
    }
    CHECK(!spsc_push(&r, &v)); /* full across the wrap */
    CHECK_EQ(spsc_count(&r), 4);
    for (uint32_t i = 0; i < 4; i++) {
        CHECK(spsc_pop(&r, &v));
        CHECK_EQ(v, i);
    }
    CHECK_EQ(spsc_count(&r), 0);
}

typedef struct {
    uint16_t a;
    uint32_t b;
    uint16_t c;
} elem8_t;

static void test_multibyte_elements(void)
{
    spsc_ring_t r;
    elem8_t storage[8];
    elem8_t in = {0x1234, 0xDEADBEEF, 0xABCD};
    elem8_t out = {0, 0, 0};

    CHECK(spsc_init(&r, storage, sizeof(elem8_t), 8));
    CHECK(spsc_push(&r, &in));
    CHECK(spsc_pop(&r, &out));
    CHECK_EQ(out.a, in.a);
    CHECK_EQ(out.b, in.b);
    CHECK_EQ(out.c, in.c);
}

static void test_peek_release_batch(void)
{
    spsc_ring_t r;
    uint32_t storage[8];
    uint32_t batch[8];

    CHECK(spsc_init(&r, storage, sizeof(uint32_t), 8));
    for (uint32_t i = 0; i < 6; i++) {
        CHECK(spsc_push(&r, &i));
    }
    CHECK_EQ(spsc_peek(&r, batch, 4), 4);
    CHECK_EQ(batch[0], 0);
    CHECK_EQ(batch[3], 3);
    CHECK_EQ(spsc_count(&r), 6); /* peek does not remove */

    spsc_release(&r, 4);
    CHECK_EQ(spsc_count(&r), 2);
    CHECK_EQ(spsc_peek(&r, batch, 8), 2);
    CHECK_EQ(batch[0], 4);

    spsc_release(&r, 100); /* clamped to what is there */
    CHECK_EQ(spsc_count(&r), 0);
}

/* Two real threads: proves nothing is lost, duplicated or reordered. */
#define STRESS_ITEMS 10000000u

static spsc_ring_t stress_ring;
static uint32_t stress_storage[1024];

static void *stress_producer(void *arg)
{
    (void)arg;
    for (uint32_t i = 0; i < STRESS_ITEMS;) {
        if (spsc_push(&stress_ring, &i)) {
            i++;
        }
    }
    return NULL;
}

static void test_two_thread_stress(void)
{
    pthread_t producer;
    uint32_t expected = 0;
    uint32_t v;

    CHECK(spsc_init(&stress_ring, stress_storage, sizeof(uint32_t), 1024));
    CHECK_EQ(pthread_create(&producer, NULL, stress_producer, NULL), 0);
    while (expected < STRESS_ITEMS) {
        if (spsc_pop(&stress_ring, &v)) {
            CHECK_EQ(v, expected);
            expected++;
        }
    }
    CHECK_EQ(pthread_join(producer, NULL), 0);
    CHECK(!spsc_pop(&stress_ring, &v));
}

int main(void)
{
    RUN_TEST(test_init_rejects_bad_args);
    RUN_TEST(test_push_pop_order);
    RUN_TEST(test_full_rejects_push);
    RUN_TEST(test_wraps_many_times);
    RUN_TEST(test_index_overflow_at_uint32_max);
    RUN_TEST(test_multibyte_elements);
    RUN_TEST(test_peek_release_batch);
    RUN_TEST(test_two_thread_stress);
    return 0;
}
