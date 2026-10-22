/* Simulator entry point: configure the fake board, boot the firmware, run the super loop. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "sim.h"

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s --capture FILE [--baud N] [--exit-when-done]\n"
            "  --baud N          UART baud rate, 8N1 (default 115200, 0 = no pacing)\n"
            "  --exit-when-done  print stats and exit once the capture is processed\n",
            prog);
}

int main(int argc, char **argv)
{
    const char *capture = NULL;
    uint32_t    baud = 115200u;
    bool        exit_when_done = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            capture = argv[++i];
        } else if (strcmp(argv[i], "--baud") == 0 && i + 1 < argc) {
            baud = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--exit-when-done") == 0) {
            exit_when_done = true;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (capture == NULL) {
        usage(argv[0]);
        return 2;
    }
    if (!sim_uart_config(capture, baud)) {
        fprintf(stderr, "cannot read capture file: %s\n", capture);
        return 1;
    }

    if (!exit_when_done) {
        sim_console_start();
    }
    app_init();

    for (;;) {
        bool busy = app_poll();
        if (exit_when_done && sim_uart_done() && app_idle()) {
            app_print_stats();
            break;
        }
        if (!busy) {
            sim_sleep_us(100); /* like WFI: nothing to do until the next interrupt */
        }
    }
    return 0;
}
