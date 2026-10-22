#include "crc16.h"
#include "flashlog.h"
#include "parser.h"
#include "sim.h"
#include "test_assert.h"

#include <setjmp.h>
#include <string.h>

static flashlog_t fl;
static uint32_t   fake_ms;

static uint32_t clock_ms(void)
{
    return fake_ms++;
}

/* A reading whose timestamp is its id, with a correct frame CRC. */
static reading_t make(uint32_t id)
{
    reading_t r;
    uint8_t   body[8] = {FRAME_READING_LEN, FRAME_TYPE_READING, 0x34, 0x12,
                         (uint8_t)id, (uint8_t)(id >> 8), (uint8_t)(id >> 16), (uint8_t)(id >> 24)};
    uint16_t  crc = crc16(body, sizeof(body));
    memcpy(r.raw, &body[2], READING_PAYLOAD_SIZE);
    r.raw[6] = (uint8_t)(crc & 0xFFu);
    r.raw[7] = (uint8_t)(crc >> 8);
    return r;
}

static void fresh_flash(void)
{
    CHECK(sim_flash_open(NULL));
    sim_flash_power_on();
    sim_flash_set_timing(false);
}

static void boot(void)
{
    sim_flash_power_on();
    CHECK_EQ(flashlog_init(&fl, clock_ms), FLASHLOG_OK);
}

static void append_ids(uint32_t first, uint32_t count)
{
    reading_t batch[32];
    uint32_t  id = first;
    while (id < first + count) {
        uint32_t n = first + count - id;
        if (n > 32u) n = 32u;
        for (uint32_t i = 0; i < n; i++) {
            batch[i] = make(id + i);
        }
        uint32_t stored = flashlog_append(&fl, batch, n);
        CHECK(stored > 0u);
        id += stored;
    }
}

/* Collects ids newest first; returns how many. */
static uint32_t collect(uint32_t *ids, uint32_t max)
{
    flashlog_iter_t it;
    reading_t       r;
    uint32_t        n = 0;
    flashlog_iter_init(&it);
    while (n < max && flashlog_iter_next(&fl, &it, &r)) {
        ids[n++] = reading_timestamp_ms(&r);
    }
    return n;
}

static uint32_t ids[20000];

static void test_geometry_check(void)
{
    hal_flash_info_t ok = {0, 65536, 4096, 256, 1, 0xFF};
    hal_flash_info_t one_sector = {0, 4096, 4096, 256, 1, 0xFF};
    hal_flash_info_t big_write_unit = {0, 65536, 4096, 256, 32, 0xFF};
    CHECK(flashlog_geometry_ok(&ok));
    CHECK(!flashlog_geometry_ok(&one_sector));
    CHECK(!flashlog_geometry_ok(&big_write_unit));
}

static void test_empty_flash_boots(void)
{
    fresh_flash();
    boot();
    CHECK_EQ(counter_get(&fl.stats.readings), 0);
    CHECK_EQ(fl.records_per_sector, 511);
    CHECK_EQ(collect(ids, 10), 0);
}

static void test_readings_survive_reboot_newest_first(void)
{
    fresh_flash();
    boot();
    append_ids(0, 1000);
    boot();
    CHECK_EQ(counter_get(&fl.stats.readings), 1000);
    CHECK_EQ(collect(ids, 20000), 1000);
    for (uint32_t i = 0; i < 1000; i++) {
        CHECK_EQ(ids[i], 999 - i);
    }
    append_ids(1000, 10); /* logging continues after reboot */
    CHECK_EQ(collect(ids, 1), 1);
    CHECK_EQ(ids[0], 1009);
}

static void test_batch_never_crosses_page(void)
{
    reading_t batch[32];
    fresh_flash();
    boot();
    for (uint32_t i = 0; i < 32; i++) {
        batch[i] = make(i);
    }
    /* Slot 0 starts at offset 8, so the first page holds 31 records. */
    CHECK_EQ(flashlog_append(&fl, batch, 32), 31);
    CHECK_EQ(flashlog_append(&fl, batch, 32), 32);
}

static void test_full_flash_discards_oldest(void)
{
    fresh_flash();
    boot();
    uint32_t capacity = fl.records_per_sector * fl.sector_count; /* 8176 */
    append_ids(0, capacity + 100);

    uint32_t n = collect(ids, 20000);
    CHECK_EQ(n, counter_get(&fl.stats.readings));
    CHECK(n <= capacity);
    CHECK(n >= capacity - fl.records_per_sector);
    CHECK_EQ(ids[0], capacity + 99);
    for (uint32_t i = 1; i < n; i++) {
        CHECK_EQ(ids[i], ids[i - 1] - 1u);
    }
    CHECK_EQ(counter_get(&fl.stats.readings_discarded), fl.records_per_sector);

    boot();
    CHECK_EQ(collect(ids, 20000), n);
}

static void test_torn_record_skipped_and_not_reused(void)
{
    fresh_flash();
    boot();
    append_ids(0, 5);

    /* Tear record 5 by hand: half of it programmed. */
    uint8_t *raw = sim_flash_raw();
    uint32_t addr = FLASHLOG_HEADER_SIZE + 5u * READING_SIZE;
    reading_t r = make(5);
    memcpy(&raw[addr], r.raw, 3);

    boot();
    CHECK_EQ(counter_get(&fl.stats.readings), 5);
    CHECK_EQ(counter_get(&fl.stats.bad_records), 1);
    CHECK_EQ(fl.next_slot, 6);
    append_ids(6, 1);
    CHECK_EQ(collect(ids, 10), 6);
    CHECK_EQ(ids[0], 6);
    CHECK_EQ(ids[1], 4);
}

static void test_garbage_sector_erased_at_boot(void)
{
    fresh_flash();
    uint8_t *raw = sim_flash_raw();
    memset(&raw[3 * 4096 + 100], 0x00, 50); /* junk, no header */
    boot();
    CHECK_EQ(counter_get(&fl.stats.bad_sectors), 1);
    CHECK_EQ(raw[3 * 4096 + 100], 0xFF);
}

static void test_write_error_skips_slot(void)
{
    fresh_flash();
    boot();
    append_ids(0, 2);
    /* Dirty the next slot so the write needs a 0 -> 1 change. */
    sim_flash_raw()[FLASHLOG_HEADER_SIZE + 2u * READING_SIZE] = 0x00;
    reading_t r = make(2);
    CHECK_EQ(flashlog_append(&fl, &r, 1), 0);
    CHECK_EQ(counter_get(&fl.stats.write_errors), 1);
    CHECK_EQ(flashlog_append(&fl, &r, 1), 1);
    CHECK_EQ(collect(ids, 10), 3);
}

static void test_erase_all(void)
{
    fresh_flash();
    boot();
    append_ids(0, 3000);
    flashlog_erase_all_start(&fl);
    reading_t r = make(1);
    CHECK_EQ(flashlog_append(&fl, &r, 1), 0); /* refused while erasing */
    uint32_t steps = 0;
    while (flashlog_erase_step(&fl)) {
        steps++;
    }
    CHECK_EQ(steps + 1u, fl.sector_count);
    CHECK_EQ(counter_get(&fl.stats.readings), 0);
    boot();
    CHECK_EQ(counter_get(&fl.stats.readings), 0);
    append_ids(5000, 3);
    CHECK_EQ(collect(ids, 10), 3);
}

static void test_interrupted_erase_all_keeps_newest_chain(void)
{
    fresh_flash();
    boot();
    append_ids(0, 3000); /* sectors 0..5 */
    flashlog_erase_all_start(&fl);
    flashlog_erase_step(&fl); /* oldest two sectors gone */
    flashlog_erase_step(&fl);
    boot();               /* power cut mid-job */
    uint32_t n = collect(ids, 20000);
    CHECK_EQ(n, counter_get(&fl.stats.readings));
    CHECK_EQ(ids[0], 2999);
    for (uint32_t i = 1; i < n; i++) {
        CHECK_EQ(ids[i], ids[i - 1] - 1u);
    }
}

/* ---- power-cut torture ------------------------------------------------ */

#define MAX_ID 2000000u

static jmp_buf cut_jump;
static uint8_t reported[MAX_ID];
static uint32_t next_id;
static uint32_t max_reported;

static void cut_hook(void)
{
    longjmp(cut_jump, 1);
}

static uint32_t rng_state = 99u;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* Every reading reported as stored must still be there, unless it is older
 * than everything present (legitimately discarded when flash was full). */
static void verify_after_reboot(void)
{
    uint32_t n = collect(ids, 20000);
    CHECK_EQ(n, counter_get(&fl.stats.readings));
    if (max_reported == 0u) {
        return;
    }
    CHECK(n > 0u);
    for (uint32_t i = 1; i < n; i++) {
        CHECK(ids[i] < ids[i - 1]); /* newest first, no duplicates */
    }
    uint32_t oldest = ids[n - 1];
    uint32_t j = n;
    for (uint32_t id = oldest; id <= max_reported; id++) {
        while (j > 0u && ids[j - 1] < id) {
            j--;
        }
        bool present = j > 0u && ids[j - 1] == id;
        if (reported[id]) {
            CHECK(present);
        }
    }
}

static uint32_t cuts_mid_write;
static uint32_t cuts_mid_erase;

static void power_cut_round(uint32_t after_bytes, uint32_t on_erase)
{
    reading_t batch[32];

    if (setjmp(cut_jump) != 0) {
        cuts_mid_write += (after_bytes != 0u);
        cuts_mid_erase += (on_erase != 0u);
    } else {
        sim_flash_power_on();
        sim_flash_set_power_cut(after_bytes, on_erase, cut_hook);
        CHECK_EQ(flashlog_init(&fl, clock_ms), FLASHLOG_OK);
        verify_after_reboot();

        for (uint32_t batch_no = 0; batch_no < 400u; batch_no++) {
            uint32_t n = 1u + rng() % 32u;
            uint32_t base = next_id;
            /* Reserve ids before writing: a cut mid-batch can leave some of
             * them fully written, so they must never be reused. */
            next_id += n;
            CHECK(next_id < MAX_ID - 64u);
            for (uint32_t i = 0; i < n; i++) {
                batch[i] = make(base + i);
            }
            uint32_t stored = flashlog_append(&fl, batch, n);
            for (uint32_t i = 0; i < stored; i++) {
                reported[base + i] = 1;
                max_reported = base + i;
            }
        }
    }
    /* Power came back (or the round ended without a cut). */
}

static void test_power_cut_torture(void)
{
    fresh_flash();
    memset(reported, 0, sizeof(reported));
    next_id = 1;
    max_reported = 0;

    for (uint32_t i = 0; i < 1500u; i++) {
        if (i % 5u == 4u) {
            power_cut_round(0, 1u + rng() % 2u); /* cut during an erase */
        } else {
            power_cut_round(1u + rng() % 2000u, 0); /* cut mid-write */
        }
    }
    sim_flash_power_on();
    CHECK_EQ(flashlog_init(&fl, clock_ms), FLASHLOG_OK);
    verify_after_reboot();
    printf("       cuts: %u mid-write, %u mid-erase; %u ids written, %u in flash\n",
           cuts_mid_write, cuts_mid_erase, next_id, counter_get(&fl.stats.readings));
    CHECK(cuts_mid_write > 1000u);
    CHECK(cuts_mid_erase > 200u);
}

int main(void)
{
    RUN_TEST(test_geometry_check);
    RUN_TEST(test_empty_flash_boots);
    RUN_TEST(test_readings_survive_reboot_newest_first);
    RUN_TEST(test_batch_never_crosses_page);
    RUN_TEST(test_full_flash_discards_oldest);
    RUN_TEST(test_torn_record_skipped_and_not_reused);
    RUN_TEST(test_garbage_sector_erased_at_boot);
    RUN_TEST(test_write_error_skips_slot);
    RUN_TEST(test_erase_all);
    RUN_TEST(test_interrupted_erase_all_keeps_newest_chain);
    RUN_TEST(test_power_cut_torture);
    return 0;
}
