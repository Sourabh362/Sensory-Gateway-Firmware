#include "app.h"

#include <stdio.h>
#include <string.h>

#include "counter.h"
#include "flashlog.h"
#include "hal.h"
#include "parser.h"
#include "spsc_ring.h"

#define RX_RING_SIZE     4096u /* power of 2; holds > 20 ms at 921600 baud */
#define READING_Q_SIZE   512u  /* power of 2; covers the 320 ms erase-all */
#define CMD_Q_SIZE       4u
#define PARSE_BUDGET     256u  /* max bytes parsed per step */
#define WRITE_BATCH      32u   /* max readings per flash write (one page) */
#define DUMP_CHUNK       32u   /* max dump lines per step */
#define CONSOLE_LINE     64u

typedef enum { CMD_DUMP, CMD_ERASE } cmd_type_t;

typedef struct {
    cmd_type_t type;
    uint32_t   arg;
} cmd_t;

/* Queues: one producer, one consumer each. */
static uint8_t     rx_storage[RX_RING_SIZE];
static spsc_ring_t rx_ring;            /* UART ISR -> parse stage */
static reading_t   reading_storage[READING_Q_SIZE];
static spsc_ring_t reading_q;          /* parse stage -> save stage */
static cmd_t       cmd_storage[CMD_Q_SIZE];
static spsc_ring_t cmd_q;              /* console stage -> save stage */

static parser_t   parser;              /* owned by the parse stage */
static flashlog_t flog;                /* owned by the save stage */

/* Dump job state, owned by the save stage. */
static bool            dump_active;
static uint32_t        dump_left;
static flashlog_iter_t dump_it;

/* Counters: one writer each, any reader. */
static counter_t bytes_received;   /* UART ISR */
static counter_t bytes_lost;       /* UART ISR */
static counter_t ms_ticks;         /* tick ISR */
static counter_t readings_dropped; /* parse stage */
static counter_t stored_count;     /* save stage */
static counter_t last_stored_ts;   /* save stage */

/* ---- interrupts ---------------------------------------------------------- */

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

/* ---- parse stage ---------------------------------------------------------- */

static void on_reading(const reading_t *r, void *ctx)
{
    (void)ctx;
    if (!spsc_push(&reading_q, r)) {
        counter_inc(&readings_dropped); /* never block: drop whole readings, not bytes */
    }
}

bool app_parse_step(void)
{
    uint8_t  byte;
    uint32_t n = 0;

    while (n < PARSE_BUDGET && spsc_pop(&rx_ring, &byte)) {
        parser_feed(&parser, byte);
        n++;
    }
    return n > 0u;
}

/* ---- save stage (sole owner of the flash) --------------------------------- */

static void print_reading(const reading_t *r)
{
    char     line[64];
    int16_t  t = reading_temp_centi(r);
    unsigned abs_t = (unsigned)(t < 0 ? -t : t);

    snprintf(line, sizeof(line), "ts=%lu ms  temp=%s%u.%02u C\n",
             (unsigned long)reading_timestamp_ms(r), t < 0 ? "-" : "", abs_t / 100u,
             abs_t % 100u);
    hal_console_write(line);
}

static void dump_chunk(void)
{
    reading_t r;
    uint32_t  lines = 0;

    while (lines < DUMP_CHUNK && dump_left > 0u && flashlog_iter_next(&flog, &dump_it, &r)) {
        print_reading(&r);
        dump_left--;
        lines++;
    }
    if (lines < DUMP_CHUNK) {
        dump_active = false;
        hal_console_write("dump done\n");
    }
}

static void start_command(const cmd_t *cmd)
{
    if (cmd->type == CMD_DUMP) {
        dump_active = true;
        dump_left = cmd->arg;
        flashlog_iter_init(&dump_it);
    } else {
        flashlog_erase_all_start(&flog);
        hal_console_write("erasing...\n");
    }
}

static bool write_readings(void)
{
    reading_t batch[WRITE_BATCH];
    uint32_t  n = spsc_peek(&reading_q, batch, WRITE_BATCH);

    if (n == 0u) {
        return false;
    }
    uint32_t stored = flashlog_append(&flog, batch, n);
    if (stored > 0u) {
        /* Only now are these readings "stored": remove them from the queue. */
        spsc_release(&reading_q, stored);
        counter_add(&stored_count, stored);
        counter_set(&last_stored_ts, reading_timestamp_ms(&batch[stored - 1u]));
    }
    return true;
}

bool app_save_step(void)
{
    cmd_t cmd;

    if (flashlog_erasing(&flog)) {
        if (!flashlog_erase_step(&flog)) {
            hal_console_write("erase done\n");
        }
        return true;
    }
    if (dump_active) {
        dump_chunk();
        return true;
    }
    if (spsc_pop(&cmd_q, &cmd)) {
        start_command(&cmd);
        return true;
    }
    return write_readings();
}

uint32_t app_stored_count(void)
{
    return counter_get(&stored_count);
}

uint32_t app_last_stored_ts(void)
{
    return counter_get(&last_stored_ts);
}

/* ---- console stage -------------------------------------------------------- */

static void print_counter(const char *name, uint32_t value)
{
    char line[64];
    snprintf(line, sizeof(line), "%-22s %lu\n", name, (unsigned long)value);
    hal_console_write(line);
}

void app_print_stats(void)
{
    print_counter("uptime_ms", app_millis());
    hal_console_write("-- uart\n");
    print_counter("bytes_received", counter_get(&bytes_received));
    print_counter("bytes_lost", counter_get(&bytes_lost));
    hal_console_write("-- parser\n");
    print_counter("readings", counter_get(&parser.stats.readings));
    print_counter("heartbeats", counter_get(&parser.stats.heartbeats));
    print_counter("crc_errors", counter_get(&parser.stats.crc_errors));
    print_counter("len_errors", counter_get(&parser.stats.len_errors));
    print_counter("unknown_frames", counter_get(&parser.stats.unknown_frames));
    print_counter("bytes_discarded", counter_get(&parser.stats.bytes_discarded));
    hal_console_write("-- queue\n");
    print_counter("readings_dropped", counter_get(&readings_dropped));
    hal_console_write("-- flash\n");
    print_counter("readings_in_flash", counter_get(&flog.stats.readings));
    print_counter("readings_stored", counter_get(&stored_count));
    print_counter("sectors_in_use", counter_get(&flog.stats.sectors_in_use));
    print_counter("sectors_erased", counter_get(&flog.stats.sectors_erased));
    print_counter("readings_discarded", counter_get(&flog.stats.readings_discarded));
    print_counter("bad_records", counter_get(&flog.stats.bad_records));
    print_counter("bad_sectors", counter_get(&flog.stats.bad_sectors));
    print_counter("write_errors", counter_get(&flog.stats.write_errors));
    print_counter("sectors_skipped", counter_get(&flog.stats.sectors_skipped));
}

static bool parse_count(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    if (*s == '\0') {
        return false;
    }
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9' || v > 100000000u) {
            return false;
        }
        v = v * 10u + (uint32_t)(*s - '0');
    }
    *out = v;
    return true;
}

static void post(cmd_type_t type, uint32_t arg)
{
    cmd_t cmd = {type, arg};
    if (!spsc_push(&cmd_q, &cmd)) {
        hal_console_write("busy, try again\n");
    }
}

bool app_console_step(void)
{
    char     line[CONSOLE_LINE];
    uint32_t n;

    if (!hal_console_getline(line, sizeof(line))) {
        return false;
    }
    if (strcmp(line, "stats") == 0) {
        app_print_stats();
    } else if (strncmp(line, "dump ", 5) == 0 && parse_count(&line[5], &n)) {
        post(CMD_DUMP, n); /* the save stage owns the flash */
    } else if (strcmp(line, "erase") == 0) {
        post(CMD_ERASE, 0);
    } else if (line[0] != '\0') {
        hal_console_write("commands: stats | dump <n> | erase\n");
    }
    return true;
}

/* ---- init + super loop ---------------------------------------------------- */

bool app_init(void)
{
    spsc_init(&rx_ring, rx_storage, 1u, RX_RING_SIZE);
    spsc_init(&reading_q, reading_storage, sizeof(reading_t), READING_Q_SIZE);
    spsc_init(&cmd_q, cmd_storage, sizeof(cmd_t), CMD_Q_SIZE);
    parser_init(&parser, on_reading, NULL);

    hal_tick_start(app_tick_isr); /* flash timeouts need the tick */
    if (flashlog_init(&flog, app_millis) != FLASHLOG_OK) {
        return false;
    }
    hal_uart_start(app_uart_rx_isr);
    return true;
}

bool app_poll(void)
{
    bool busy = app_parse_step();
    busy |= app_save_step();
    busy |= app_console_step();
    return busy;
}

bool app_idle(void)
{
    return spsc_count(&rx_ring) == 0u && spsc_count(&reading_q) == 0u &&
           spsc_count(&cmd_q) == 0u && !dump_active && !flashlog_erasing(&flog);
}
