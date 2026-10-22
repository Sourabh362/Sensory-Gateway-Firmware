#include "hal.h"
#include "sim.h"
#include "sim_time.h"
#include "test_assert.h"

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static void fresh_ram_flash(bool timing)
{
    CHECK(sim_flash_open(NULL));
    sim_flash_power_on();
    sim_flash_set_timing(timing);
}

static void test_geometry(void)
{
    hal_flash_info_t info;
    hal_flash_get_info(&info);
    CHECK_EQ(info.size, 64u * 1024u);
    CHECK_EQ(info.sector_size, 4096u);
    CHECK_EQ(info.page_size, 256u);
    CHECK_EQ(info.erased_value, 0xFF);
}

static void test_starts_erased(void)
{
    uint8_t buf[64];
    fresh_ram_flash(false);
    CHECK_EQ(hal_flash_read(1000, buf, sizeof(buf)), HAL_OK);
    for (size_t i = 0; i < sizeof(buf); i++) {
        CHECK_EQ(buf[i], 0xFF);
    }
}

static void test_write_only_clears_bits(void)
{
    uint8_t v;
    fresh_ram_flash(false);

    v = 0xF0;
    CHECK_EQ(hal_flash_write(10, &v, 1), HAL_OK);
    v = 0x30; /* clears more bits: allowed */
    CHECK_EQ(hal_flash_write(10, &v, 1), HAL_OK);
    v = 0x31; /* would set bit 0 back to 1: rejected */
    CHECK_EQ(hal_flash_write(10, &v, 1), HAL_ERR_BITS);
    CHECK_EQ(hal_flash_read(10, &v, 1), HAL_OK);
    CHECK_EQ(v, 0x30);
}

static void test_rejected_write_changes_nothing(void)
{
    uint8_t data[4] = {0x00, 0x00, 0x00, 0x00};
    uint8_t back[4];
    fresh_ram_flash(false);

    CHECK_EQ(hal_flash_write(100, data, 1), HAL_OK); /* byte 100 = 0x00 */
    uint8_t mixed[4] = {0x0F, 0x0F, 0x0F, 0x0F};     /* byte 100 needs 0 -> 1 */
    CHECK_EQ(hal_flash_write(100, mixed, 4), HAL_ERR_BITS);
    CHECK_EQ(hal_flash_read(100, back, 4), HAL_OK);
    CHECK_EQ(back[0], 0x00);
    CHECK_EQ(back[1], 0xFF);
    CHECK_EQ(back[3], 0xFF);
}

static void test_erase_restores_ff(void)
{
    uint8_t zeros[256];
    uint8_t v;
    memset(zeros, 0, sizeof(zeros));
    fresh_ram_flash(false);

    CHECK_EQ(hal_flash_write(4096, zeros, sizeof(zeros)), HAL_OK);
    CHECK_EQ(hal_flash_write(8192, zeros, 1), HAL_OK); /* next sector */
    CHECK_EQ(hal_flash_erase_sector(4096), HAL_OK);
    CHECK_EQ(hal_flash_read(4096 + 200, &v, 1), HAL_OK);
    CHECK_EQ(v, 0xFF);
    CHECK_EQ(hal_flash_read(8192, &v, 1), HAL_OK);
    CHECK_EQ(v, 0x00); /* other sector untouched */
}

static void test_argument_checks(void)
{
    uint8_t buf[300];
    memset(buf, 0, sizeof(buf));
    fresh_ram_flash(false);

    CHECK_EQ(hal_flash_write(0, buf, 0), HAL_ERR_ARG);
    CHECK_EQ(hal_flash_write(0, buf, 257), HAL_ERR_ARG);           /* > page */
    CHECK_EQ(hal_flash_write(250, buf, 10), HAL_ERR_ARG);          /* crosses page */
    CHECK_EQ(hal_flash_write(65535, buf, 2), HAL_ERR_ARG);         /* past end */
    CHECK_EQ(hal_flash_erase_sector(100), HAL_ERR_ARG);            /* unaligned */
    CHECK_EQ(hal_flash_erase_sector(64u * 1024u), HAL_ERR_ARG);    /* out of range */
    CHECK_EQ(hal_flash_read(65530, buf, 10), HAL_ERR_ARG);
    CHECK_EQ(hal_flash_write(248, buf, 8), HAL_OK);                /* ends at page edge */
}

static void test_busy_timing(void)
{
    uint8_t v = 0;
    fresh_ram_flash(true);

    uint64_t t0 = sim_now_ns();
    CHECK_EQ(hal_flash_erase_sector(0), HAL_OK);
    CHECK(hal_flash_busy());
    CHECK_EQ(hal_flash_write(0, &v, 1), HAL_ERR_BUSY);
    CHECK_EQ(hal_flash_read(0, &v, 1), HAL_ERR_BUSY);
    while (hal_flash_busy()) {
    }
    uint64_t erase_ns = sim_now_ns() - t0;
    CHECK(erase_ns >= 20000000u);

    t0 = sim_now_ns();
    CHECK_EQ(hal_flash_write(0, &v, 1), HAL_OK);
    while (hal_flash_busy()) {
    }
    uint64_t write_ns = sim_now_ns() - t0;
    CHECK(write_ns >= 100000u);
    CHECK(write_ns < 20000000u);
}

static void test_persists_in_file(void)
{
    const char *path = "test_flash_sim.img";
    uint8_t     data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t     back[8];

    remove(path);
    CHECK(sim_flash_open(path));
    sim_flash_power_on();
    sim_flash_set_timing(false);
    CHECK_EQ(hal_flash_read(4096, back, 1), HAL_OK);
    CHECK_EQ(back[0], 0xFF); /* new file starts erased */
    CHECK_EQ(hal_flash_write(4096, data, sizeof(data)), HAL_OK);
    sim_flash_close();

    CHECK(sim_flash_open(path)); /* "restart" */
    CHECK_EQ(hal_flash_read(4096, back, sizeof(back)), HAL_OK);
    CHECK(memcmp(back, data, sizeof(data)) == 0);
    sim_flash_close();
    remove(path);
}

static jmp_buf cut_jump;

static void cut_hook(void)
{
    longjmp(cut_jump, 1);
}

static void test_power_cut_tears_write(void)
{
    uint8_t zeros[8] = {0};
    uint8_t back[8];
    fresh_ram_flash(false);

    sim_flash_set_power_cut(3, 0, cut_hook);
    if (setjmp(cut_jump) == 0) {
        hal_flash_write(0, zeros, sizeof(zeros));
        CHECK(0); /* must not get here */
    }
    sim_flash_power_on();
    CHECK_EQ(hal_flash_read(0, back, sizeof(back)), HAL_OK);
    CHECK_EQ(back[0], 0x00);
    CHECK_EQ(back[2], 0x00); /* exactly 3 bytes programmed */
    CHECK_EQ(back[3], 0xFF);
    CHECK_EQ(back[7], 0xFF);
}

static void test_power_cut_during_erase(void)
{
    uint8_t zeros[256];
    uint8_t v;
    memset(zeros, 0, sizeof(zeros));
    fresh_ram_flash(false);

    for (uint32_t a = 0; a < 4096; a += 256) {
        CHECK_EQ(hal_flash_write(a, zeros, sizeof(zeros)), HAL_OK);
    }
    sim_flash_set_power_cut(0, 1, cut_hook);
    if (setjmp(cut_jump) == 0) {
        hal_flash_erase_sector(0);
        CHECK(0);
    }
    sim_flash_power_on();
    CHECK_EQ(hal_flash_read(0, &v, 1), HAL_OK);
    CHECK_EQ(v, 0xFF); /* first half erased */
    CHECK_EQ(hal_flash_read(4095, &v, 1), HAL_OK);
    CHECK_EQ(v, 0x00); /* second half still old data */
}

int main(void)
{
    RUN_TEST(test_geometry);
    RUN_TEST(test_starts_erased);
    RUN_TEST(test_write_only_clears_bits);
    RUN_TEST(test_rejected_write_changes_nothing);
    RUN_TEST(test_erase_restores_ff);
    RUN_TEST(test_argument_checks);
    RUN_TEST(test_busy_timing);
    RUN_TEST(test_persists_in_file);
    RUN_TEST(test_power_cut_tears_write);
    RUN_TEST(test_power_cut_during_erase);
    return 0;
}
