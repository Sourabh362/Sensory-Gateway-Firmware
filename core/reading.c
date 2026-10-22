#include "reading.h"

#include "crc16.h"
#include "parser.h"

int16_t reading_temp_centi(const reading_t *r)
{
    return (int16_t)(uint16_t)(r->raw[0] | (r->raw[1] << 8));
}

uint32_t reading_timestamp_ms(const reading_t *r)
{
    return (uint32_t)r->raw[2] | ((uint32_t)r->raw[3] << 8) |
           ((uint32_t)r->raw[4] << 16) | ((uint32_t)r->raw[5] << 24);
}

bool reading_crc_ok(const reading_t *r)
{
    uint16_t crc = CRC16_INIT;
    crc = crc16_update(crc, FRAME_READING_LEN);
    crc = crc16_update(crc, FRAME_TYPE_READING);
    for (uint32_t i = 0; i < READING_PAYLOAD_SIZE; i++) {
        crc = crc16_update(crc, r->raw[i]);
    }
    uint16_t stored = (uint16_t)(r->raw[6] | (r->raw[7] << 8));
    return crc == stored;
}
