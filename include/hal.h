/*
 * hal.h - hardware abstraction layer.
 *
 * The ONLY way the firmware in core/ reaches hardware. On the PC these
 * functions are implemented by the simulator in sim/; on a microcontroller
 * they would wrap the vendor drivers (UART ISR, SysTick, SPI/internal flash).
 */
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

/* ---------------------------------------------------------------------------
 * Interrupts
 *
 * The firmware registers its handlers; the HAL calls them from interrupt
 * context. Handlers must never block, wait or allocate.
 * ------------------------------------------------------------------------- */
typedef void (*hal_uart_rx_isr_t)(uint8_t byte);
typedef void (*hal_tick_isr_t)(void);

/* Start UART reception; isr is called once per received byte. */
void hal_uart_start(hal_uart_rx_isr_t isr);

/* Start the 1 ms system tick; isr is called every millisecond. */
void hal_tick_start(hal_tick_isr_t isr);

/* ---------------------------------------------------------------------------
 * Flash (NOR-style: erase sets bits to 1, write can only clear bits to 0)
 *
 * Geometry is reported by the HAL so the log works on any chip/partition.
 * Addresses are absolute; the log only uses [base, base + size).
 *
 * erase/write only START an operation. Poll hal_flash_busy() until false
 * before the next operation, like polling a status register.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t base;         /* first address the log may use */
    uint32_t size;         /* bytes available to the log */
    uint32_t sector_size;  /* erase unit */
    uint32_t page_size;    /* max bytes per write call */
    uint32_t write_unit;   /* smallest programmable unit (1, 8, 32 ...) */
    uint8_t  erased_value; /* value of an erased byte, usually 0xFF */
} hal_flash_info_t;

void         hal_flash_get_info(hal_flash_info_t *info);
hal_status_t hal_flash_erase_sector(uint32_t sector_addr);
hal_status_t hal_flash_write(uint32_t addr, const void *data, size_t len);
hal_status_t hal_flash_read(uint32_t addr, void *data, size_t len);
bool         hal_flash_busy(void);

/* ---------------------------------------------------------------------------
 * Debug console
 * ------------------------------------------------------------------------- */

/* Non-blocking. Copies one complete line (without the newline) into buf and
 * returns true, or returns false immediately if no full line is ready. */
bool hal_console_getline(char *buf, size_t size);

/* Writes text to the console output. */
void hal_console_write(const char *text);

#endif /* HAL_H */
