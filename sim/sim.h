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

void sim_sleep_us(uint32_t us);

#endif /* SIM_H */
