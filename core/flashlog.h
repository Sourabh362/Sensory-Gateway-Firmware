/*
 * flashlog.h - power-safe reading log on NOR flash.
 *
 * Flash holds only: sector headers (magic, sector seq, crc) and 8 B records.
 * All state and counters live in RAM and are rebuilt by scanning at boot.
 * Single owner: only the save stage may call these functions.
 */
#ifndef FLASHLOG_H
#define FLASHLOG_H

#include <stdbool.h>
#include <stdint.h>

#include "counter.h"
#include "hal.h"
#include "reading.h"

#define FLASHLOG_HEADER_SIZE 8u
#define FLASHLOG_MAGIC       0x4C47u

typedef uint32_t (*flashlog_clock_t)(void); /* milliseconds, for timeouts */

/* RAM only. */
typedef struct {
    counter_t readings;           /* valid readings currently in flash */
    counter_t sectors_in_use;
    counter_t sectors_erased;
    counter_t readings_discarded; /* oldest dropped when a sector was recycled */
    counter_t bad_records;        /* torn/corrupt records found at boot */
    counter_t bad_sectors;        /* garbage/stale sectors erased at boot */
    counter_t write_errors;
    counter_t sectors_skipped;    /* abandoned after repeated failures */
} flashlog_stats_t;

typedef struct {
    hal_flash_info_t info;
    uint32_t         sector_count;
    uint32_t         records_per_sector;
    flashlog_clock_t now_ms;

    bool     has_active;
    uint32_t active;     /* sector being written */
    uint32_t active_seq;
    uint32_t last_seq;   /* last sector seq handed out */
    uint32_t next_slot;  /* next free record slot in the active sector */
    uint32_t slot_failures;

    bool     erasing;    /* erase-all job in progress */
    uint32_t erase_next;
    uint32_t erase_left;

    flashlog_stats_t stats;
} flashlog_t;

typedef enum {
    FLASHLOG_OK = 0,
    FLASHLOG_ERR_GEOMETRY,
    FLASHLOG_ERR_FLASH,
} flashlog_status_t;

/* Newest-first iterator for dump. */
typedef struct {
    uint32_t sectors_done;
    uint32_t slot;
    bool     started;
} flashlog_iter_t;

bool              flashlog_geometry_ok(const hal_flash_info_t *info);
flashlog_status_t flashlog_init(flashlog_t *fl, flashlog_clock_t now_ms);

/* Stores up to n readings in as few writes as possible (max one page per call).
 * Returns how many were stored; only those count as "stored". */
uint32_t flashlog_append(flashlog_t *fl, const reading_t *r, uint32_t n);

void flashlog_erase_all_start(flashlog_t *fl);
bool flashlog_erase_step(flashlog_t *fl); /* true while more steps remain */
bool flashlog_erasing(const flashlog_t *fl);

void flashlog_iter_init(flashlog_iter_t *it);
bool flashlog_iter_next(flashlog_t *fl, flashlog_iter_t *it, reading_t *out);

#endif /* FLASHLOG_H */
