/* app.h - firmware entry points: init, ISRs and the main-loop step. */
#ifndef APP_H
#define APP_H

#include <stdbool.h>
#include <stdint.h>

void app_init(void);

/* One pass of the main loop. Returns true if any work was done. */
bool app_poll(void);

/* True when no received bytes are waiting to be processed. */
bool app_idle(void);

void     app_print_stats(void);
uint32_t app_millis(void);

/* Interrupt handlers registered with the HAL. */
void app_uart_rx_isr(uint8_t byte);
void app_tick_isr(void);

#endif /* APP_H */
