#include "flashlog.h"

#include <string.h>

#include "crc16.h"

#define ERASE_TIMEOUT_MS   200u
#define WRITE_TIMEOUT_MS   20u
#define MAX_SLOT_FAILURES  3u
#define MAX_BATCH          32u /* records per write: one 256 B page */

/* ---- flash access ------------------------------------------------------- */

static bool wait_ready(flashlog_t *fl, uint32_t timeout_ms)
{
    uint32_t start = fl->now_ms();
    while (hal_flash_busy()) {
        if (fl->now_ms() - start > timeout_ms) {
            return false;
        }
    }
    return true;
}

static uint32_t sector_addr(const flashlog_t *fl, uint32_t sector)
{
    return fl->info.base + sector * fl->info.sector_size;
}

static uint32_t slot_addr(const flashlog_t *fl, uint32_t sector, uint32_t slot)
{
    return sector_addr(fl, sector) + FLASHLOG_HEADER_SIZE + slot * READING_SIZE;
}

static bool read_bytes(flashlog_t *fl, uint32_t addr, void *buf, uint32_t len)
{
    return wait_ready(fl, ERASE_TIMEOUT_MS) && hal_flash_read(addr, buf, len) == HAL_OK;
}

static bool is_blank(const flashlog_t *fl, const uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        if (buf[i] != fl->info.erased_value) {
            return false;
        }
    }
    return true;
}

static bool sector_blank(flashlog_t *fl, uint32_t sector)
{
    uint8_t chunk[64];
    for (uint32_t off = 0; off < fl->info.sector_size; off += sizeof(chunk)) {
        if (!read_bytes(fl, sector_addr(fl, sector) + off, chunk, sizeof(chunk)) ||
            !is_blank(fl, chunk, sizeof(chunk))) {
            return false;
        }
    }
    return true;
}

static bool erase_sector(flashlog_t *fl, uint32_t sector)
{
    if (!wait_ready(fl, ERASE_TIMEOUT_MS) ||
        hal_flash_erase_sector(sector_addr(fl, sector)) != HAL_OK ||
        !wait_ready(fl, ERASE_TIMEOUT_MS)) {
        counter_inc(&fl->stats.write_errors);
        return false;
    }
    counter_inc(&fl->stats.sectors_erased);
    return true;
}

static bool write_bytes(flashlog_t *fl, uint32_t addr, const void *buf, uint32_t len)
{
    if (!wait_ready(fl, WRITE_TIMEOUT_MS) || hal_flash_write(addr, buf, len) != HAL_OK ||
        !wait_ready(fl, WRITE_TIMEOUT_MS)) {
        counter_inc(&fl->stats.write_errors);
        return false;
    }
    return true;
}

/* Encryption hook (off): would transform the 8-byte record in place. */
static void record_encode(reading_t *r) { (void)r; }
static void record_decode(reading_t *r) { (void)r; }

/* ---- header ------------------------------------------------------------- */

static void header_encode(uint8_t out[FLASHLOG_HEADER_SIZE], uint32_t seq)
{
    out[0] = (uint8_t)(FLASHLOG_MAGIC & 0xFFu);
    out[1] = (uint8_t)(FLASHLOG_MAGIC >> 8);
    out[2] = (uint8_t)(seq & 0xFFu);
    out[3] = (uint8_t)((seq >> 8) & 0xFFu);
    out[4] = (uint8_t)((seq >> 16) & 0xFFu);
    out[5] = (uint8_t)(seq >> 24);
    uint16_t crc = crc16(out, 6);
    out[6] = (uint8_t)(crc & 0xFFu);
    out[7] = (uint8_t)(crc >> 8);
}

/* Returns true and the seq if the sector has a valid header. */
static bool header_read(flashlog_t *fl, uint32_t sector, uint32_t *seq)
{
    uint8_t h[FLASHLOG_HEADER_SIZE];
    if (!read_bytes(fl, sector_addr(fl, sector), h, sizeof(h))) {
        return false;
    }
    uint16_t magic = (uint16_t)(h[0] | (h[1] << 8));
    uint16_t crc = (uint16_t)(h[6] | (h[7] << 8));
    if (magic != FLASHLOG_MAGIC || crc16(h, 6) != crc) {
        return false;
    }
    *seq = (uint32_t)h[2] | ((uint32_t)h[3] << 8) | ((uint32_t)h[4] << 16) |
           ((uint32_t)h[5] << 24);
    return true;
}

/* ---- records ------------------------------------------------------------ */

typedef enum { SLOT_BLANK, SLOT_VALID, SLOT_BAD } slot_state_t;

static slot_state_t slot_read(flashlog_t *fl, uint32_t sector, uint32_t slot, reading_t *out)
{
    if (!read_bytes(fl, slot_addr(fl, sector, slot), out->raw, READING_SIZE)) {
        return SLOT_BAD;
    }
    if (is_blank(fl, out->raw, READING_SIZE)) {
        return SLOT_BLANK;
    }
    record_decode(out);
    return reading_crc_ok(out) ? SLOT_VALID : SLOT_BAD;
}

/* Counts valid and bad records; *used = index after the last non-blank slot. */
static uint32_t scan_sector(flashlog_t *fl, uint32_t sector, uint32_t *bad, uint32_t *used)
{
    uint32_t  valid = 0;
    reading_t r;

    *bad = 0;
    *used = 0;
    for (uint32_t s = 0; s < fl->records_per_sector; s++) {
        slot_state_t st = slot_read(fl, sector, s, &r);
        if (st != SLOT_BLANK) {
            *used = s + 1u;
        }
        valid += (st == SLOT_VALID);
        *bad += (st == SLOT_BAD);
    }
    return valid;
}

/* ---- geometry + boot ---------------------------------------------------- */

bool flashlog_geometry_ok(const hal_flash_info_t *info)
{
    return info->sector_size > FLASHLOG_HEADER_SIZE && info->size % info->sector_size == 0u &&
           info->size / info->sector_size >= 2u && info->page_size >= READING_SIZE &&
           info->page_size % READING_SIZE == 0u && info->write_unit != 0u &&
           READING_SIZE % info->write_unit == 0u &&
           (info->sector_size - FLASHLOG_HEADER_SIZE) % READING_SIZE == 0u;
}

static uint32_t ring_prev(const flashlog_t *fl, uint32_t sector, uint32_t k)
{
    return (sector + fl->sector_count - (k % fl->sector_count)) % fl->sector_count;
}

flashlog_status_t flashlog_init(flashlog_t *fl, flashlog_clock_t now_ms)
{
    memset(fl, 0, sizeof(*fl));
    fl->now_ms = now_ms;
    hal_flash_get_info(&fl->info);
    if (!flashlog_geometry_ok(&fl->info)) {
        return FLASHLOG_ERR_GEOMETRY;
    }
    fl->sector_count = fl->info.size / fl->info.sector_size;
    fl->records_per_sector = (fl->info.sector_size - FLASHLOG_HEADER_SIZE) / READING_SIZE;

    /* 1. Newest valid header = active sector. */
    uint32_t seq;
    for (uint32_t s = 0; s < fl->sector_count; s++) {
        if (header_read(fl, s, &seq) && (!fl->has_active || seq > fl->active_seq)) {
            fl->has_active = true;
            fl->active = s;
            fl->active_seq = seq;
        }
    }

    /* 2. Walk back while seq is contiguous: that is the chain of live data. */
    uint32_t chain = 0;
    if (fl->has_active) {
        chain = 1;
        while (chain < fl->sector_count &&
               header_read(fl, ring_prev(fl, fl->active, chain), &seq) &&
               seq == fl->active_seq - chain) {
            chain++;
        }
        fl->last_seq = fl->active_seq;
    }

    /* 3. Anything outside the chain that is not blank is a torn erase/header
     *    or stale data: erase it so it can be reused cleanly. */
    for (uint32_t k = chain; k < fl->sector_count; k++) {
        uint32_t s = fl->has_active ? ring_prev(fl, fl->active, k) : k;
        if (!sector_blank(fl, s)) {
            counter_inc(&fl->stats.bad_sectors);
            if (!erase_sector(fl, s)) {
                return FLASHLOG_ERR_FLASH;
            }
        }
    }

    /* 4. Count live records; resume after the last used slot of the active sector. */
    uint32_t total = 0;
    uint32_t bad_total = 0;
    for (uint32_t k = 0; k < chain; k++) {
        uint32_t bad;
        uint32_t used;
        total += scan_sector(fl, ring_prev(fl, fl->active, k), &bad, &used);
        bad_total += bad;
        if (k == 0u) {
            fl->next_slot = used;
        }
    }
    counter_set(&fl->stats.readings, total);
    counter_set(&fl->stats.bad_records, bad_total);
    counter_set(&fl->stats.sectors_in_use, chain);
    return FLASHLOG_OK;
}

/* ---- append ------------------------------------------------------------- */

/* Moves to the next sector in ring order, recycling the oldest if needed. */
static bool open_next_sector(flashlog_t *fl)
{
    uint32_t next = fl->has_active ? (fl->active + 1u) % fl->sector_count : 0u;

    for (uint32_t attempt = 0; attempt < fl->sector_count; attempt++) {
        uint32_t seq;
        if (header_read(fl, next, &seq)) {
            /* Oldest sector of the chain: its readings are discarded. */
            uint32_t bad;
            uint32_t used;
            uint32_t valid = scan_sector(fl, next, &bad, &used);
            counter_set(&fl->stats.readings, counter_get(&fl->stats.readings) - valid);
            counter_add(&fl->stats.readings_discarded, valid);
            counter_set(&fl->stats.sectors_in_use, counter_get(&fl->stats.sectors_in_use) - 1u);
        }

        uint8_t hdr[FLASHLOG_HEADER_SIZE];
        header_encode(hdr, fl->last_seq + 1u);

        /* Order matters for power safety: erase -> header -> records. */
        if ((sector_blank(fl, next) || erase_sector(fl, next)) &&
            write_bytes(fl, sector_addr(fl, next), hdr, sizeof(hdr))) {
            fl->last_seq++;
            fl->has_active = true;
            fl->active = next;
            fl->active_seq = fl->last_seq;
            fl->next_slot = 0;
            fl->slot_failures = 0;
            counter_add(&fl->stats.sectors_in_use, 1u);
            return true;
        }
        counter_inc(&fl->stats.sectors_skipped);
        next = (next + 1u) % fl->sector_count;
    }
    return false;
}

uint32_t flashlog_append(flashlog_t *fl, const reading_t *r, uint32_t n)
{
    reading_t batch[MAX_BATCH];

    if (n == 0u || fl->erasing) {
        return 0;
    }
    if (!fl->has_active || fl->next_slot >= fl->records_per_sector) {
        if (!open_next_sector(fl)) {
            return 0;
        }
    }

    /* Batch size: limited by request, sector space and the page boundary. */
    uint32_t addr = slot_addr(fl, fl->active, fl->next_slot);
    uint32_t k = n;
    uint32_t sector_room = fl->records_per_sector - fl->next_slot;
    uint32_t page_room = (fl->info.page_size - addr % fl->info.page_size) / READING_SIZE;
    if (k > sector_room) k = sector_room;
    if (k > page_room) k = page_room;
    if (k > MAX_BATCH) k = MAX_BATCH;

    memcpy(batch, r, k * sizeof(reading_t));
    for (uint32_t i = 0; i < k; i++) {
        record_encode(&batch[i]);
    }

    if (!write_bytes(fl, addr, batch, k * READING_SIZE)) {
        /* Slots may be partly programmed: never reuse them. */
        fl->next_slot += k;
        if (++fl->slot_failures >= MAX_SLOT_FAILURES) {
            fl->next_slot = fl->records_per_sector; /* give up on this sector */
            counter_inc(&fl->stats.sectors_skipped);
        }
        return 0;
    }
    fl->next_slot += k;
    counter_add(&fl->stats.readings, k);
    return k;
}

/* ---- erase all (one sector per step) ------------------------------------ */

void flashlog_erase_all_start(flashlog_t *fl)
{
    uint32_t in_use = counter_get(&fl->stats.sectors_in_use);

    /* Oldest first, so an interrupted erase leaves the newest data as a valid chain. */
    fl->erase_next = (fl->has_active && in_use > 0u) ? ring_prev(fl, fl->active, in_use - 1u) : 0u;
    fl->erase_left = fl->sector_count;
    fl->erasing = true;
}

bool flashlog_erasing(const flashlog_t *fl)
{
    return fl->erasing;
}

bool flashlog_erase_step(flashlog_t *fl)
{
    if (!fl->erasing) {
        return false;
    }
    uint32_t s = fl->erase_next;
    uint32_t seq;
    if (header_read(fl, s, &seq)) {
        uint32_t bad;
        uint32_t used;
        uint32_t valid = scan_sector(fl, s, &bad, &used);
        counter_set(&fl->stats.readings, counter_get(&fl->stats.readings) - valid);
        counter_set(&fl->stats.sectors_in_use, counter_get(&fl->stats.sectors_in_use) - 1u);
    }
    if (!sector_blank(fl, s)) {
        erase_sector(fl, s);
    }
    fl->erase_next = (s + 1u) % fl->sector_count;
    if (--fl->erase_left == 0u) {
        fl->erasing = false;
        fl->has_active = false;
        fl->next_slot = 0;
        counter_set(&fl->stats.readings, 0);
        counter_set(&fl->stats.sectors_in_use, 0);
    }
    return fl->erasing;
}

/* ---- newest-first iteration --------------------------------------------- */

void flashlog_iter_init(flashlog_iter_t *it)
{
    memset(it, 0, sizeof(*it));
}

bool flashlog_iter_next(flashlog_t *fl, flashlog_iter_t *it, reading_t *out)
{
    uint32_t in_use = counter_get(&fl->stats.sectors_in_use);

    if (!fl->has_active) {
        return false;
    }
    if (!it->started) {
        it->started = true;
        it->slot = fl->next_slot;
    }
    while (it->sectors_done < in_use) {
        uint32_t sector = ring_prev(fl, fl->active, it->sectors_done);
        while (it->slot > 0u) {
            it->slot--;
            if (slot_read(fl, sector, it->slot, out) == SLOT_VALID) {
                return true;
            }
        }
        it->sectors_done++;
        it->slot = fl->records_per_sector;
    }
    return false;
}
