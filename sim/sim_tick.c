/* 1 ms tick simulation: a thread plays the timer interrupt. */
#include <pthread.h>

#include "hal.h"
#include "sim_time.h"

static hal_tick_isr_t tick_isr;

static void *tick_thread(void *arg)
{
    (void)arg;
    uint64_t next = sim_now_ns();

    for (;;) {
        next += 1000000u;
        uint64_t now = sim_now_ns();
        if (next > now) {
            sim_sleep_us((uint32_t)((next - now) / 1000u));
        }
        tick_isr();
    }
    return NULL;
}

void hal_tick_start(hal_tick_isr_t isr)
{
    pthread_t thread;

    tick_isr = isr;
    pthread_create(&thread, NULL, tick_thread, NULL);
    pthread_detach(thread);
}
