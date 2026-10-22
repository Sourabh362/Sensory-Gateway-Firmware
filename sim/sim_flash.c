/*
 * NOR flash simulation: 64 KB, 4 KB sectors, 256 B pages.
 * Erase sets a sector to 0xFF (20 ms); a write can only clear bits (100 us).
 * File-backed via mmap(MAP_SHARED), so programmed bytes survive a hard kill.
 */
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hal.h"
#include "sim_time.h"

#define FLASH_SIZE     (64u * 1024u)
#define SECTOR_SIZE    (4u * 1024u)
#define PAGE_SIZE      256u
#define ERASED         0xFFu
#define ERASE_TIME_NS  20000000u
#define WRITE_TIME_NS  100000u

static uint8_t  ram_flash[FLASH_SIZE];
static uint8_t *mem = ram_flash;
static int      fd = -1;

static bool     timing_enabled = true;
static uint64_t busy_until_ns;

static uint32_t             cut_after_bytes;
static uint32_t             cut_on_erase;
static sim_power_cut_hook_t cut_hook;
static uint32_t             bytes_programmed;
static uint32_t             erase_count;

bool sim_flash_open(const char *path)
{
    sim_flash_close();
    if (path == NULL) {
        memset(ram_flash, ERASED, sizeof(ram_flash));
        mem = ram_flash;
        return true;
    }

    fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return false;
    }
    struct stat st;
    bool fresh = fstat(fd, &st) != 0 || st.st_size != (off_t)FLASH_SIZE;
    if (fresh && ftruncate(fd, FLASH_SIZE) != 0) {
        close(fd);
        fd = -1;
        return false;
    }
    void *p = mmap(NULL, FLASH_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        fd = -1;
        return false;
    }
    mem = p;
    if (fresh) {
        memset(mem, ERASED, FLASH_SIZE); /* factory-fresh chip */
    }
    return true;
}

void sim_flash_close(void)
{
    if (fd >= 0) {
        munmap(mem, FLASH_SIZE);
        close(fd);
        fd = -1;
    }
    mem = ram_flash;
}

void sim_flash_set_timing(bool enabled)
{
    timing_enabled = enabled;
}

uint8_t *sim_flash_raw(void)
{
    return mem;
}

void sim_flash_set_power_cut(uint32_t after_bytes, uint32_t on_erase, sim_power_cut_hook_t hook)
{
    cut_after_bytes = after_bytes;
    cut_on_erase = on_erase;
    cut_hook = hook;
    bytes_programmed = 0;
    erase_count = 0;
}

void sim_flash_power_on(void)
{
    busy_until_ns = 0;
    sim_flash_set_power_cut(0, 0, NULL);
}

static void power_cut(void)
{
    busy_until_ns = 0;
    if (cut_hook != NULL) {
        cut_hook(); /* does not return in the gateway (process exits) */
    }
}

static void start_busy(uint32_t ns)
{
    busy_until_ns = timing_enabled ? sim_now_ns() + ns : 0;
}

bool hal_flash_busy(void)
{
    return timing_enabled && sim_now_ns() < busy_until_ns;
}

void hal_flash_get_info(hal_flash_info_t *info)
{
    info->base = 0;
    info->size = FLASH_SIZE;
    info->sector_size = SECTOR_SIZE;
    info->page_size = PAGE_SIZE;
    info->write_unit = 1;
    info->erased_value = ERASED;
}

hal_status_t hal_flash_erase_sector(uint32_t sector_addr)
{
    if (sector_addr >= FLASH_SIZE || sector_addr % SECTOR_SIZE != 0u) {
        return HAL_ERR_ARG;
    }
    if (hal_flash_busy()) {
        return HAL_ERR_BUSY;
    }

    erase_count++;
    if (cut_on_erase != 0u && erase_count == cut_on_erase) {
        memset(&mem[sector_addr], ERASED, SECTOR_SIZE / 2u); /* interrupted erase */
        power_cut();
    }
    memset(&mem[sector_addr], ERASED, SECTOR_SIZE);
    start_busy(ERASE_TIME_NS);
    return HAL_OK;
}

hal_status_t hal_flash_write(uint32_t addr, const void *data, size_t len)
{
    const uint8_t *src = data;

    if (len == 0u || len > PAGE_SIZE || addr >= FLASH_SIZE || len > FLASH_SIZE - addr ||
        addr / PAGE_SIZE != (addr + (uint32_t)len - 1u) / PAGE_SIZE) {
        return HAL_ERR_ARG; /* empty, too long, out of range or crosses a page */
    }
    if (hal_flash_busy()) {
        return HAL_ERR_BUSY;
    }
    for (size_t i = 0; i < len; i++) {
        if ((src[i] & (uint8_t)~mem[addr + i]) != 0u) {
            return HAL_ERR_BITS; /* would need a 0 -> 1 change */
        }
    }

    /* Program byte by byte so a power cut can tear the write. */
    for (size_t i = 0; i < len; i++) {
        mem[addr + i] &= src[i];
        bytes_programmed++;
        if (cut_after_bytes != 0u && bytes_programmed == cut_after_bytes) {
            power_cut();
        }
    }
    start_busy(WRITE_TIME_NS);
    return HAL_OK;
}

hal_status_t hal_flash_read(uint32_t addr, void *data, size_t len)
{
    if (addr >= FLASH_SIZE || len > FLASH_SIZE - addr) {
        return HAL_ERR_ARG;
    }
    if (hal_flash_busy()) {
        return HAL_ERR_BUSY;
    }
    memcpy(data, &mem[addr], len);
    return HAL_OK;
}
