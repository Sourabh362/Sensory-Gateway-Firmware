#include "crc16.h"
#include "parser.h"
#include "reading.h"
#include "test_assert.h"

#include <string.h>

#define MAX_CAPTURED 32768

static reading_t captured[MAX_CAPTURED];
static uint32_t  n_captured;

static void capture(const reading_t *r, void *ctx)
{
    (void)ctx;
    if (n_captured < MAX_CAPTURED) {
        captured[n_captured] = *r;
    }
    n_captured++;
}

static parser_t parser;

static void reset(void)
{
    n_captured = 0;
    parser_init(&parser, capture, NULL);
}

static void feed(const uint8_t *bytes, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        parser_feed(&parser, bytes[i]);
    }
}

/* Builds a frame with a correct CRC; returns its length. */
static size_t make_frame(uint8_t *out, uint8_t type, const uint8_t *payload, uint8_t len)
{
    out[0] = FRAME_SOF;
    out[1] = len;
    out[2] = type;
    memcpy(&out[3], payload, len);
    uint16_t crc = crc16(&out[1], (size_t)len + 2u);
    out[3 + len] = (uint8_t)(crc & 0xFFu);
    out[4 + len] = (uint8_t)(crc >> 8);
    return (size_t)len + 5u;
}

static size_t make_reading(uint8_t *out, int16_t temp, uint32_t ts)
{
    uint8_t p[6] = {
        (uint8_t)((uint16_t)temp & 0xFFu), (uint8_t)((uint16_t)temp >> 8),
        (uint8_t)(ts & 0xFFu), (uint8_t)((ts >> 8) & 0xFFu),
        (uint8_t)((ts >> 16) & 0xFFu), (uint8_t)(ts >> 24),
    };
    return make_frame(out, FRAME_TYPE_READING, p, 6);
}

static const uint8_t EX_READING_1[] = {0xAA, 0x06, 0x01, 0x29, 0x09, 0xE8, 0x03,
                                       0x00, 0x00, 0x7B, 0x84};
static const uint8_t EX_READING_2[] = {0xAA, 0x06, 0x01, 0x00, 0xFE, 0x70, 0x11,
                                       0x01, 0x00, 0x7D, 0xFC};
static const uint8_t EX_HEARTBEAT[] = {0xAA, 0x00, 0x02, 0x4D, 0x3D};

static void test_crc_check_value(void)
{
    CHECK_EQ(crc16((const uint8_t *)"123456789", 9), 0x29B1);
}

static void test_brief_example_frames(void)
{
    reset();
    feed(EX_READING_1, sizeof(EX_READING_1));
    feed(EX_READING_2, sizeof(EX_READING_2));
    feed(EX_HEARTBEAT, sizeof(EX_HEARTBEAT));

    CHECK_EQ(counter_get(&parser.stats.readings), 2);
    CHECK_EQ(counter_get(&parser.stats.heartbeats), 1);
    CHECK_EQ(counter_get(&parser.stats.bytes_discarded), 0);
    CHECK_EQ(n_captured, 2);
    CHECK_EQ(reading_temp_centi(&captured[0]), 2345);
    CHECK_EQ(reading_timestamp_ms(&captured[0]), 1000);
    CHECK_EQ(reading_temp_centi(&captured[1]), -512);
    CHECK_EQ(reading_timestamp_ms(&captured[1]), 70000);
}

static void test_reading_keeps_frame_crc(void)
{
    reset();
    feed(EX_READING_1, sizeof(EX_READING_1));
    CHECK_EQ(captured[0].raw[6], 0x7B);
    CHECK_EQ(captured[0].raw[7], 0x84);
    CHECK(reading_crc_ok(&captured[0]));

    captured[0].raw[2] ^= 0x01u;
    CHECK(!reading_crc_ok(&captured[0]));
}

static void test_blank_record_never_valid(void)
{
    reading_t blank;
    memset(blank.raw, 0xFF, sizeof(blank.raw));
    CHECK(!reading_crc_ok(&blank));
}

static void test_junk_bytes_discarded(void)
{
    static const uint8_t junk[] = {0x00, 0x37, 0x91, 0xFF, 0x0C};
    reset();
    feed(junk, sizeof(junk));
    feed(EX_READING_1, sizeof(EX_READING_1));
    CHECK_EQ(counter_get(&parser.stats.bytes_discarded), sizeof(junk));
    CHECK_EQ(counter_get(&parser.stats.readings), 1);
}

static void test_frame_hidden_after_false_sof(void)
{
    /* AA 05 makes the parser collect a fake 5-byte payload that swallows
     * the start of the real frame; the rescan must still find it. */
    static const uint8_t noise[] = {0x37, 0xAA, 0x05};
    reset();
    feed(noise, sizeof(noise));
    feed(EX_READING_1, sizeof(EX_READING_1));
    CHECK_EQ(counter_get(&parser.stats.readings), 1);
    CHECK_EQ(counter_get(&parser.stats.crc_errors), 1);
    CHECK_EQ(reading_timestamp_ms(&captured[0]), 1000);
}

static void test_sof_directly_before_frame_is_length_error(void)
{
    static const uint8_t noise[] = {0xAA};
    reset();
    feed(noise, sizeof(noise));
    feed(EX_READING_1, sizeof(EX_READING_1)); /* LEN would be 0xAA > 64 */
    CHECK_EQ(counter_get(&parser.stats.len_errors), 1);
    CHECK_EQ(counter_get(&parser.stats.readings), 1);
}

static void test_truncated_frame_then_good_frame(void)
{
    reset();
    feed(EX_READING_2, 6); /* first half only */
    feed(EX_READING_1, sizeof(EX_READING_1));
    feed(EX_HEARTBEAT, sizeof(EX_HEARTBEAT));
    CHECK_EQ(counter_get(&parser.stats.readings), 1);
    CHECK_EQ(counter_get(&parser.stats.heartbeats), 1);
    CHECK_EQ(reading_timestamp_ms(&captured[0]), 1000);
}

static void test_corrupted_frame_counted_not_stored(void)
{
    uint8_t bad[sizeof(EX_READING_1)];
    memcpy(bad, EX_READING_1, sizeof(bad));
    bad[5] ^= 0x10u;

    reset();
    feed(bad, sizeof(bad));
    feed(EX_HEARTBEAT, sizeof(EX_HEARTBEAT));
    CHECK_EQ(counter_get(&parser.stats.readings), 0);
    CHECK_EQ(counter_get(&parser.stats.crc_errors), 1);
    CHECK_EQ(counter_get(&parser.stats.heartbeats), 1);
    CHECK_EQ(n_captured, 0);
}

static void test_length_over_64_rejected(void)
{
    static const uint8_t bad[] = {0xAA, 0x41, 0x01};
    reset();
    feed(bad, sizeof(bad));
    feed(EX_HEARTBEAT, sizeof(EX_HEARTBEAT));
    CHECK_EQ(counter_get(&parser.stats.len_errors), 1);
    CHECK_EQ(counter_get(&parser.stats.heartbeats), 1);
}

static void test_unknown_type_and_wrong_len_counted(void)
{
    uint8_t buf[80];
    uint8_t payload[64] = {0};
    size_t  n;

    reset();
    n = make_frame(buf, 0x07, payload, 3); /* unknown type */
    feed(buf, n);
    n = make_frame(buf, FRAME_TYPE_READING, payload, 4); /* reading with LEN 4 */
    feed(buf, n);
    n = make_frame(buf, FRAME_TYPE_HEARTBEAT, payload, 2); /* heartbeat with payload */
    feed(buf, n);
    n = make_frame(buf, 0x09, payload, 64); /* max length */
    feed(buf, n);
    feed(EX_READING_1, sizeof(EX_READING_1));

    CHECK_EQ(counter_get(&parser.stats.unknown_frames), 4);
    CHECK_EQ(counter_get(&parser.stats.readings), 1);
    CHECK_EQ(n_captured, 1);
}

static void test_sof_value_inside_payload(void)
{
    uint8_t buf[16];
    size_t  n = make_reading(buf, (int16_t)0x00AA, 0xAAAAAAAAu);

    reset();
    feed(buf, n);
    CHECK_EQ(counter_get(&parser.stats.readings), 1);
    CHECK_EQ(reading_temp_centi(&captured[0]), 0xAA);
    CHECK_EQ(reading_timestamp_ms(&captured[0]), 0xAAAAAAAAu);
}

/* Deterministic noise generator (xorshift32). */
static uint32_t rng_state = 12345u;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

#define TRUNCATED_TS 0xFFFFFFFFu

static void test_random_noise_finds_every_intact_frame(void)
{
    static uint8_t stream[200000];
    size_t   len = 0;
    uint32_t good = 0;
    uint8_t  frame[16];

    while (len < sizeof(stream) - 64u) {
        uint32_t noise = rng() % 8u; /* 0..7 junk bytes, often containing 0xAA */
        for (uint32_t i = 0; i < noise; i++) {
            stream[len++] = (rng() % 4u == 0u) ? FRAME_SOF : (uint8_t)rng();
        }
        size_t n;
        if (rng() % 10u == 0u) {
            /* Truncated frame, marked so it can be told apart: a cut that drops
             * only the last CRC byte can be completed by chance by the next
             * byte, and then it genuinely is a valid frame. */
            n = make_reading(frame, 0, TRUNCATED_TS);
            n = rng() % n;
        } else {
            n = make_reading(frame, (int16_t)rng(), good);
            good++;
        }
        memcpy(&stream[len], frame, n);
        len += n;
    }

    reset();
    feed(stream, len);
    CHECK(n_captured <= MAX_CAPTURED);

    /* Intact frames carry their index as timestamp: all must appear, in order. */
    uint32_t found = 0;
    uint32_t extras = 0;
    for (uint32_t i = 0; i < n_captured; i++) {
        uint32_t ts = reading_timestamp_ms(&captured[i]);
        if (ts == found) {
            found++;
        } else {
            CHECK_EQ(ts, TRUNCATED_TS);
            extras++;
        }
    }
    CHECK_EQ(found, good);
    CHECK_EQ(counter_get(&parser.stats.readings), good + extras);
}

int main(void)
{
    RUN_TEST(test_crc_check_value);
    RUN_TEST(test_brief_example_frames);
    RUN_TEST(test_reading_keeps_frame_crc);
    RUN_TEST(test_blank_record_never_valid);
    RUN_TEST(test_junk_bytes_discarded);
    RUN_TEST(test_frame_hidden_after_false_sof);
    RUN_TEST(test_sof_directly_before_frame_is_length_error);
    RUN_TEST(test_truncated_frame_then_good_frame);
    RUN_TEST(test_corrupted_frame_counted_not_stored);
    RUN_TEST(test_length_over_64_rejected);
    RUN_TEST(test_unknown_type_and_wrong_len_counted);
    RUN_TEST(test_sof_value_inside_payload);
    RUN_TEST(test_random_noise_finds_every_intact_frame);
    return 0;
}
