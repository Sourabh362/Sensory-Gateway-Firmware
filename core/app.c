#include "app.h"

#include <stdio.h>
#include <string.h>

#include "counter.h"
#include "hal.h"
#include "parser.h"
#include "spsc_ring.h"

#define RX_RING_SIZE   4096u /* power of 2; holds > 20 ms at 921600 baud */
#define PARSE_BUDGET   256u  /* max bytes parsed per step */
#define CONSOLE_LINE   64u

static uint8_t     rx_storage[RX_RING_SIZE];
static spsc_ring_t rx_ring;
static parser_t    parser;

/* Written only by the ISRs. */
static counter_t bytes_received;
static counter_t bytes_lost;
static counter_t ms_ticks;

void app_uart_rx_isr(uint8_t byte)
{
    /* On hardware, byte comes from the volatile UART data register. */
    counter_inc(&bytes_received);
    if (!spsc_push(&rx_ring, &byte)) {
        counter_inc(&bytes_lost);
    }
}

void app_tick_isr(void)
{
    counter_inc(&ms_ticks);
}

uint32_t app_millis(void)
{
    return counter_get(&ms_ticks);
}

void app_init(void)
{
    spsc_init(&rx_ring, rx_storage, 1u, RX_RING_SIZE);
    parser_init(&parser, NULL, NULL);
    hal_tick_start(app_tick_isr);
    hal_uart_start(app_uart_rx_isr);
}

static bool parse_step(void)
{
    uint8_t  byte;
    uint32_t n = 0;

    while (n < PARSE_BUDGET && spsc_pop(&rx_ring, &byte)) {
        parser_feed(&parser, byte);
        n++;
    }
    return n > 0u;
}

static void print_counter(const char *name, uint32_t value)
{
    char line[64];
    snprintf(line, sizeof(line), "%-18s %lu\n", name, (unsigned long)value);
    hal_console_write(line);
}

void app_print_stats(void)
{
    print_counter("uptime_ms", app_millis());
    print_counter("bytes_received", counter_get(&bytes_received));
    print_counter("bytes_lost", counter_get(&bytes_lost));
    print_counter("readings", counter_get(&parser.stats.readings));
    print_counter("heartbeats", counter_get(&parser.stats.heartbeats));
    print_counter("crc_errors", counter_get(&parser.stats.crc_errors));
    print_counter("len_errors", counter_get(&parser.stats.len_errors));
    print_counter("unknown_frames", counter_get(&parser.stats.unknown_frames));
    print_counter("bytes_discarded", counter_get(&parser.stats.bytes_discarded));
}

static bool console_step(void)
{
    char line[CONSOLE_LINE];

    if (!hal_console_getline(line, sizeof(line))) {
        return false;
    }
    if (strcmp(line, "stats") == 0) {
        app_print_stats();
    } else if (line[0] != '\0') {
        hal_console_write("unknown command\n");
    }
    return true;
}

bool app_poll(void)
{
    bool busy = parse_step();
    busy |= console_step();
    return busy;
}

bool app_idle(void)
{
    return spsc_count(&rx_ring) == 0u;
}
