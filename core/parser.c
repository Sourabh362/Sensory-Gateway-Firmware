#include "parser.h"

#include <string.h>

#include "crc16.h"

typedef enum
{
    STEP_MORE,   /* frame in progress */
    STEP_DONE,   /* byte or frame consumed, start over */
    STEP_REJECT, /* candidate frame is bad: drop SOF, rescan the rest */
} step_result_t;

void parser_init(parser_t *p, parser_reading_cb_t on_reading, void *ctx)
{
    memset(p, 0, sizeof(*p));
    p->state = PARSER_SOF;
    p->on_reading = on_reading;
    p->ctx = ctx;
}

static step_result_t finish_frame(parser_t *p)
{
    uint8_t len = p->frame[1];
    uint8_t type = p->frame[2];
    uint16_t rx_crc = (uint16_t)(p->frame[p->len - 2] | (p->frame[p->len - 1] << 8));

    if (crc16(&p->frame[1], (size_t)len + 2u) != rx_crc)
    {
        counter_inc(&p->stats.crc_errors);
        return STEP_REJECT;
    }

    if (type == FRAME_TYPE_READING && len == FRAME_READING_LEN)
    {

        reading_t r;
        memcpy(r.raw, &p->frame[FRAME_HEADER_SIZE], READING_SIZE); /* payload + crc */
        int16_t t = reading_temp_centi(&r);
        if (t < -4000 || t > 8500)
        {
            counter_inc(&p->stats.temp_out_of_range);
            return STEP_DONE;
        }
        if (p->on_reading != NULL)
        {
            p->on_reading(&r, p->ctx);
        }
        return STEP_DONE;
    }

    else if(type == FRAME_TYPE_HUMIDITY && len == FRAME_READING_LEN){
        reading_t r;
        memcpy(r.raw, &p->frame[FRAME_HEADER_SIZE], READING_SIZE); /* payload + crc */

        if (p->on_reading != NULL)
        {
            p->on_reading(&r, p->ctx);
        }
         counter_inc(&p->stats.humidity);
        return STEP_DONE;
    }

    else if (type == FRAME_TYPE_HEARTBEAT && len == 0u)
    {
        counter_inc(&p->stats.heartbeats);
    }
    else
    {
        counter_inc(&p->stats.unknown_frames);
    }
    return STEP_DONE;
}

static step_result_t step(parser_t *p, uint8_t b)
{
    switch (p->state)
    {
    case PARSER_SOF:
        if (b != FRAME_SOF)
        {
            counter_inc(&p->stats.bytes_discarded);
            return STEP_DONE;
        }
        p->frame[0] = b;
        p->len = 1;
        p->state = PARSER_LEN;
        return STEP_MORE;

    case PARSER_LEN:
        p->frame[p->len++] = b;
        if (b > FRAME_MAX_PAYLOAD)
        {
            counter_inc(&p->stats.len_errors);
            return STEP_REJECT;
        }
        p->state = PARSER_TYPE;
        return STEP_MORE;

    case PARSER_TYPE:
        p->frame[p->len++] = b;
        p->state = (p->frame[1] == 0u) ? PARSER_CRC_LO : PARSER_PAYLOAD;
        return STEP_MORE;

    case PARSER_PAYLOAD:
        p->frame[p->len++] = b;
        if (p->len == FRAME_HEADER_SIZE + p->frame[1])
        {
            p->state = PARSER_CRC_LO;
        }
        return STEP_MORE;

    case PARSER_CRC_LO:
        p->frame[p->len++] = b;
        p->state = PARSER_CRC_HI;
        return STEP_MORE;

    case PARSER_CRC_HI:
        p->frame[p->len++] = b;
        return finish_frame(p);
    }
    return STEP_DONE;
}

/*
 * Bytes in flight (candidate frame + pending replay) never exceed
 * FRAME_MAX_SIZE: each step moves one byte from pending into the frame,
 * and each reject drops one byte.
 */
void parser_feed(parser_t *p, uint8_t byte)
{
    uint8_t pending[FRAME_MAX_SIZE];
    uint32_t count = 0;
    uint32_t pos = 0;

    pending[count++] = byte;

    while (pos < count)
    {
        step_result_t res = step(p, pending[pos++]);
        if (res == STEP_MORE)
        {
            continue;
        }
        if (res == STEP_REJECT)
        {
            uint8_t replay[FRAME_MAX_SIZE];
            uint32_t from_frame = p->len - 1u; /* everything after the false SOF */
            uint32_t unread = count - pos;

            counter_inc(&p->stats.bytes_discarded);
            memcpy(replay, &p->frame[1], from_frame);
            memcpy(&replay[from_frame], &pending[pos], unread);
            count = from_frame + unread;
            memcpy(pending, replay, count);
            pos = 0;
        }
        p->state = PARSER_SOF;
        p->len = 0;
    }
}
