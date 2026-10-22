/*
 * UART RX simulation: a thread plays the RX interrupt, delivering the capture
 * one byte at a time at baud/10 bytes per second (8N1). Bytes are released in
 * small bursts every ~100 us, like a UART FIFO, but the average rate is exact.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "hal.h"
#include "sim_time.h"

#define BITS_PER_BYTE 10u

static uint8_t          *capture;
static size_t            capture_size;
static uint32_t          capture_baud;
static hal_uart_rx_isr_t rx_isr;
static atomic_bool       done;

bool sim_uart_config(const char *capture_path, uint32_t baud)
{
    FILE *f = fopen(capture_path, "rb");
    if (f == NULL) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    capture = malloc(size > 0 ? (size_t)size : 1u);
    capture_size = (size > 0) ? fread(capture, 1, (size_t)size, f) : 0u;
    fclose(f);
    capture_baud = baud;
    return capture != NULL;
}

bool sim_uart_done(void)
{
    return atomic_load(&done);
}

size_t sim_uart_size(void)
{
    return capture_size;
}

static void *uart_thread(void *arg)
{
    (void)arg;
    uint64_t start = sim_now_ns();
    size_t   sent = 0;

    while (sent < capture_size) {
        size_t due = capture_size;
        if (capture_baud != 0u) {
            uint64_t elapsed = sim_now_ns() - start;
            due = (size_t)(elapsed * capture_baud / (BITS_PER_BYTE * 1000000000ull)) + 1u;
            if (due > capture_size) {
                due = capture_size;
            }
        }
        while (sent < due) {
            rx_isr(capture[sent++]);
        }
        if (capture_baud != 0u) {
            sim_sleep_us(100);
        }
    }
    atomic_store(&done, true);
    return NULL;
}

void hal_uart_start(hal_uart_rx_isr_t isr)
{
    pthread_t thread;

    rx_isr = isr;
    if (capture == NULL) {
        atomic_store(&done, true);
        return;
    }
    pthread_create(&thread, NULL, uart_thread, NULL);
    pthread_detach(thread);
}
