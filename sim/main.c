/* Simulator entry point: configure the fake board, boot the firmware, run the super loop. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app.h"
#include "sim.h"

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s --capture FILE [options]\n"
            "  --baud N             UART baud rate, 8N1 (default 115200, 0 = no pacing)\n"
            "  --flash FILE         flash image (default flash.img)\n"
            "  --cut-after-bytes N  power cut after N flash bytes programmed\n"
            "  --cut-on-erase K     power cut halfway through erase number K\n"
            "  --exit-when-done     print stats and exit once everything is stored\n",
            prog);
}

static void power_cut(void)
{
    printf("\n*** POWER CUT *** last reading reported stored: ts=%lu ms (%lu stored this run)\n",
           (unsigned long)app_last_stored_ts(), (unsigned long)app_stored_count());
    fflush(stdout);
    _exit(3); /* like losing power: no cleanup, flash file keeps what was programmed */
}

int main(int argc, char **argv)
{
    const char *capture = NULL;
    const char *flash = "flash.img";
    uint32_t    baud = 115200u;
    uint32_t    cut_bytes = 0;
    uint32_t    cut_erase = 0;
    bool        exit_when_done = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            capture = argv[++i];
        } else if (strcmp(argv[i], "--baud") == 0 && i + 1 < argc) {
            baud = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--flash") == 0 && i + 1 < argc) {
            flash = argv[++i];
        } else if (strcmp(argv[i], "--cut-after-bytes") == 0 && i + 1 < argc) {
            cut_bytes = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--cut-on-erase") == 0 && i + 1 < argc) {
            cut_erase = (uint32_t)strtoul(argv[++i], NULL, 10);
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
    if (!sim_flash_open(flash)) {
        fprintf(stderr, "cannot open flash image: %s\n", flash);
        return 1;
    }
    sim_flash_set_power_cut(cut_bytes, cut_erase, power_cut);

    if (!exit_when_done) {
        sim_console_start();
    }
    if (!app_init(!exit_when_done)) {
        fprintf(stderr, "flash log failed to start\n");
        return 1;
    }

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
    sim_flash_close();
    return 0;
}
