/*
 * Byte-at-a-time UART frame parser: AA | LEN | TYPE | PAYLOAD | CRC16 LE.
 * On a bad frame it drops only the SOF and rescans the following bytes, so a
 * good frame starting inside a damaged one is still found.
 */
#ifndef PARSER_H
#define PARSER_H

#include <stdint.h>

#include "counter.h"
#include "reading.h"

#define FRAME_SOF            0xAAu
#define FRAME_MAX_PAYLOAD    64u
#define FRAME_TYPE_READING   0x01u
#define FRAME_TYPE_HEARTBEAT 0x02u
#define FRAME_TYPE_HUMIDITY  0x03u
#define FRAME_READING_LEN    6u
#define FRAME_HEADER_SIZE    3u /* SOF, LEN, TYPE */
#define FRAME_MAX_SIZE       (FRAME_HEADER_SIZE + FRAME_MAX_PAYLOAD + 2u)

typedef struct {
    counter_t readings;
    counter_t heartbeats;
    counter_t humidity;
    counter_t crc_errors;
    counter_t len_errors;
    counter_t temp_out_of_range;
    counter_t unknown_frames;  /* valid CRC, unknown TYPE or wrong LEN for TYPE */
    counter_t bytes_discarded; /* bytes skipped looking for SOF + dropped false SOFs */
} parser_stats_t;

typedef void (*parser_reading_cb_t)(const reading_t *r, void *ctx);

typedef enum {
    PARSER_SOF,
    PARSER_LEN,
    PARSER_TYPE,
    PARSER_PAYLOAD,
    PARSER_CRC_LO,
    PARSER_CRC_HI,
} parser_state_t;

typedef struct {
    parser_state_t      state;
    uint8_t             frame[FRAME_MAX_SIZE]; /* candidate frame bytes so far */
    uint32_t            len;
    parser_stats_t      stats;
    parser_reading_cb_t on_reading;
    void               *ctx;
} parser_t;

void parser_init(parser_t *p, parser_reading_cb_t on_reading, void *ctx);
void parser_feed(parser_t *p, uint8_t byte);

#endif /* PARSER_H */
