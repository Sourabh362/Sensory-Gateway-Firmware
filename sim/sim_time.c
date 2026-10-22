#include "sim_time.h"

#include <time.h>

uint64_t sim_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void sim_sleep_us(uint32_t us)
{
    struct timespec ts = {
        .tv_sec = (time_t)(us / 1000000u),
        .tv_nsec = (long)(us % 1000000u) * 1000L,
    };
    nanosleep(&ts, NULL);
}
