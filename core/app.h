/* app.h - firmware entry points: init, ISRs and the main-loop stages. */
#ifndef APP_H
#define APP_H

#include <stdbool.h>
#include <stdint.h>

/* Returns false if the flash log cannot start (bad geometry or flash fault). */
bool app_init(void);

/* Stages: each does a bounded amount of work and returns true if it did any.
 * Single core: call all three in a loop. Multi-core: any split works, as long
 * as each stage runs in only one context. */
bool app_parse_step(void);
bool app_save_step(void);
bool app_console_step(void);

/* Convenience for a single-core super loop: all three stages once. */
bool app_poll(void);

/* True when nothing is pending in any queue or job. */
bool app_idle(void);

void     app_print_stats(void);
uint32_t app_millis(void);

/* Readings stored this session, and the timestamp of the last one. */
uint32_t app_stored_count(void);
uint32_t app_last_stored_ts(void);

/* Interrupt handlers registered with the HAL. */
void app_uart_rx_isr(uint8_t byte);
void app_tick_isr(void);

#endif /* APP_H */
