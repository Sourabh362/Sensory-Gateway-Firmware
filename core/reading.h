/*
 * A stored reading: the frame payload (temp + timestamp) followed by the
 * frame's own CRC, exactly as received. This is also the 8-byte flash record.
 */
#ifndef READING_H
#define READING_H

#include <stdbool.h>
#include <stdint.h>

#define READING_PAYLOAD_SIZE 6u
#define READING_SIZE         8u

typedef struct {
    uint8_t raw[READING_SIZE]; /* temp i16 LE | timestamp u32 LE | crc u16 LE */
} reading_t;

int16_t  reading_temp_centi(const reading_t *r);
uint32_t reading_timestamp_ms(const reading_t *r);

/* Recomputes the frame CRC over LEN=06, TYPE=01 and the payload. */
bool reading_crc_ok(const reading_t *r);

#endif /* READING_H */
