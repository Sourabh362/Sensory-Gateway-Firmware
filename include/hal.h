/* hal.h - the only interface between the firmware (core/) and hardware. */
#ifndef HAL_H
#define HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    HAL_OK = 0,
    HAL_ERR_ARG,   /* bad address, length or alignment */
    HAL_ERR_BUSY,  /* previous flash operation still running */
    HAL_ERR_BITS,  /* write tried to change a bit from 0 to 1 */
    HAL_ERR_IO,    /* device fault (e.g. ECC error on read) */
} hal_status_t;

/* Interrupts: handlers must never block, wait or allocate. */
typedef void (*hal_uart_rx_isr_t)(uint8_t byte);
typedef void (*hal_tick_isr_t)(void);

void hal_uart_start(hal_uart_rx_isr_t isr); /* isr called per received byte */
void hal_tick_start(hal_tick_isr_t isr);    /* isr called every 1 ms */

/* Flash: erase/write only start an operation; poll hal_flash_busy() until false. */
typedef struct {
    uint32_t base;
    uint32_t size;
    uint32_t sector_size;
    uint32_t page_size;    /* max bytes per write */
    uint32_t write_unit;   /* smallest programmable unit */
    uint8_t  erased_value; /* usually 0xFF */
} hal_flash_info_t;

void         hal_flash_get_info(hal_flash_info_t *info);
hal_status_t hal_flash_erase_sector(uint32_t sector_addr);
hal_status_t hal_flash_write(uint32_t addr, const void *data, size_t len);
hal_status_t hal_flash_read(uint32_t addr, void *data, size_t len);
bool         hal_flash_busy(void);

/* Console: getline is non-blocking, returns false if no full line yet. */
bool hal_console_getline(char *buf, size_t size);
void hal_console_write(const char *text);

#endif /* HAL_H */
