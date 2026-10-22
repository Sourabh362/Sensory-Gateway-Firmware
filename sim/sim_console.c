/*
 * Console simulation: a thread blocks on stdin and hands each complete line
 * to the firmware through a one-slot mailbox, so hal_console_getline() never
 * blocks.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "hal.h"
#include "sim_time.h"

#define LINE_MAX_LEN 128

static char        mailbox[LINE_MAX_LEN];
static atomic_bool mailbox_full;

static void *console_thread(void *arg)
{
    (void)arg;
    char line[LINE_MAX_LEN];

    while (fgets(line, sizeof(line), stdin) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        while (atomic_load_explicit(&mailbox_full, memory_order_acquire)) {
            sim_sleep_us(1000);
        }
        memcpy(mailbox, line, sizeof(line));
        atomic_store_explicit(&mailbox_full, true, memory_order_release);
    }
    return NULL;
}

void sim_console_start(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, console_thread, NULL);
    pthread_detach(thread);
}

bool hal_console_getline(char *buf, size_t size)
{
    if (!atomic_load_explicit(&mailbox_full, memory_order_acquire)) {
        return false;
    }
    snprintf(buf, size, "%s", mailbox);
    atomic_store_explicit(&mailbox_full, false, memory_order_release);
    return true;
}

void hal_console_write(const char *text)
{
    fputs(text, stdout);
    fflush(stdout);
}
