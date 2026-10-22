/* sim.h - simulator-only controls. Never included by core/. */
#ifndef SIM_H
#define SIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Load the capture file; baud 0 delivers bytes as fast as possible. */
bool   sim_uart_config(const char *capture_path, uint32_t baud);
bool   sim_uart_done(void);
size_t sim_uart_size(void);

void sim_console_start(void);

/* Flash: path NULL keeps it in RAM only (tests); otherwise persisted via mmap. */
bool     sim_flash_open(const char *path);
void     sim_flash_close(void);
void     sim_flash_set_timing(bool enabled);
uint8_t *sim_flash_raw(void);

/* Power cut: hook runs after `after_bytes` programmed bytes, or halfway through
 * erase number `on_erase` (1-based). 0 disables a trigger. */
typedef void (*sim_power_cut_hook_t)(void);
void sim_flash_set_power_cut(uint32_t after_bytes, uint32_t on_erase, sim_power_cut_hook_t hook);

/* Simulates a reboot of the flash chip: clears busy state and disarms the cut. */
void sim_flash_power_on(void);

void sim_sleep_us(uint32_t us);

#endif /* SIM_H */
