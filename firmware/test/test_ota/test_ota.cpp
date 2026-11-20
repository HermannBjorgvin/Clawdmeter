// ota_proto: the firmware-update frames, the receive logic and the ring.
//   pio test -d firmware -e native -f test_ota
#include <unity.h>
#include <string.h>
#include <vector>
#include "ota_proto.h"

void setUp(void) {}
void tearDown(void) {}

static std::vector<uint8_t> begin_frame(uint32_t size, const char* env) {
    std::vector<uint8_t> f = {OTA_OP_BEGIN, (uint8_t)size, (uint8_t)(size >> 8), (uint8_t)(size >> 16),
                              (uint8_t)(size >> 24)};
    for (int i = 0; i < OTA_SHA_LEN; i++) f.push_back((uint8_t)i);
    f.insert(f.end(), env, env + strlen(env));
    return f;
}

static void test_begin_parses_and_checks(void) {
    std::vector<uint8_t> f = begin_frame(1858528, "waveshare_amoled_216");
    OtaBegin b;
    TEST_ASSERT_TRUE(ota_parse_begin(f.data(), f.size(), &b));
    TEST_ASSERT_EQUAL_UINT32(1858528, b.size);
    TEST_ASSERT_EQUAL_UINT8(31, b.sha[31]);
    TEST_ASSERT_EQUAL_STRING("waveshare_amoled_216", b.env);

    TEST_ASSERT_EQUAL(OTA_ERR_NONE, ota_check_begin(&b, "waveshare_amoled_216", 0x330000));
    TEST_ASSERT_EQUAL(OTA_ERR_NO_SLOT, ota_check_begin(&b, "waveshare_amoled_216", 0));
    TEST_ASSERT_EQUAL(OTA_ERR_WRONG_BOARD, ota_check_begin(&b, "waveshare_lcd_154", 0x330000));
    TEST_ASSERT_EQUAL(OTA_ERR_TOO_BIG, ota_check_begin(&b, "waveshare_amoled_216", 1858527));
    b.size = 0;
    TEST_ASSERT_EQUAL(OTA_ERR_TOO_BIG, ota_check_begin(&b, "waveshare_amoled_216", 0x330000));
}

static void test_begin_rejects_bad_frames(void) {
    OtaBegin b;
    std::vector<uint8_t> f = begin_frame(100, "");
    TEST_ASSERT_FALSE_MESSAGE(ota_parse_begin(f.data(), f.size(), &b), "no env");
    f = begin_frame(100, "an env with spaces");
    TEST_ASSERT_FALSE(ota_parse_begin(f.data(), f.size(), &b));
    f = begin_frame(100, "a_very_long_env_name_that_runs_past_forty_chars");
    TEST_ASSERT_FALSE(ota_parse_begin(f.data(), f.size(), &b));
    f = begin_frame(100, "sim");
    f[0] = OTA_OP_DATA;
    TEST_ASSERT_FALSE(ota_parse_begin(f.data(), f.size(), &b));
    TEST_ASSERT_FALSE(ota_parse_begin(f.data(), 10, &b));
}

static void test_data_frames(void) {
    const uint8_t f[] = {OTA_OP_DATA, 0x00, 0x10, 0x00, 0x00, 0xAA, 0xBB};
    uint32_t off;
    const uint8_t* p;
    size_t n;
    TEST_ASSERT_TRUE(ota_parse_data(f, sizeof(f), &off, &p, &n));
    TEST_ASSERT_EQUAL_UINT32(4096, off);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_UINT8(0xAA, p[0]);
    TEST_ASSERT_FALSE_MESSAGE(ota_parse_data(f, OTA_DATA_HEADER, &off, &p, &n), "header only");
    const uint8_t g[] = {OTA_OP_END, 0, 0, 0, 0, 1};
    TEST_ASSERT_FALSE(ota_parse_data(g, sizeof(g), &off, &p, &n));
}

static void test_chunks_in_order_gap_and_duplicate(void) {
    OtaRx rx;
    ota_rx_start(&rx, 1000);
    bool resend;
    TEST_ASSERT_EQUAL(OTA_CHUNK_TAKE, ota_rx_chunk(&rx, 0, 100, 500, &resend));
    TEST_ASSERT_FALSE(resend);
    ota_rx_took(&rx, 100);
    // Chunk at 100 lost: the next ones ask for 100, once.
    TEST_ASSERT_EQUAL(OTA_CHUNK_GAP, ota_rx_chunk(&rx, 200, 100, 500, &resend));
    TEST_ASSERT_TRUE(resend);
    TEST_ASSERT_EQUAL(OTA_CHUNK_GAP, ota_rx_chunk(&rx, 300, 100, 500, &resend));
    TEST_ASSERT_FALSE_MESSAGE(resend, "one RESEND per lost offset");
    // The host rewinds: an old one in flight is a duplicate, then 100 lands.
    TEST_ASSERT_EQUAL(OTA_CHUNK_DUPLICATE, ota_rx_chunk(&rx, 0, 100, 500, &resend));
    TEST_ASSERT_EQUAL(OTA_CHUNK_TAKE, ota_rx_chunk(&rx, 100, 100, 500, &resend));
    ota_rx_took(&rx, 100);
    // A later loss asks again.
    TEST_ASSERT_EQUAL(OTA_CHUNK_GAP, ota_rx_chunk(&rx, 400, 100, 500, &resend));
    TEST_ASSERT_TRUE(resend);
    TEST_ASSERT_EQUAL(OTA_CHUNK_OVERRUN, ota_rx_chunk(&rx, 950, 100, 500, &resend));
}

static void test_full_buffer_asks_to_resend(void) {
    OtaRx rx;
    ota_rx_start(&rx, 1000);
    bool resend;
    TEST_ASSERT_EQUAL(OTA_CHUNK_FULL, ota_rx_chunk(&rx, 0, 100, 50, &resend));
    TEST_ASSERT_TRUE(resend);
    TEST_ASSERT_EQUAL(OTA_CHUNK_FULL, ota_rx_chunk(&rx, 0, 100, 50, &resend));
    TEST_ASSERT_FALSE(resend);
    TEST_ASSERT_EQUAL(OTA_CHUNK_TAKE, ota_rx_chunk(&rx, 0, 100, 100, &resend));
}

static void test_ack_cadence(void) {
    TEST_ASSERT_FALSE(ota_ack_due(1000, 0, 100000));
    TEST_ASSERT_TRUE(ota_ack_due(OTA_ACK_STEP, 0, 100000));
    TEST_ASSERT_FALSE(ota_ack_due(OTA_ACK_STEP + 10, OTA_ACK_STEP, 100000));
    TEST_ASSERT_TRUE_MESSAGE(ota_ack_due(100000, 99000, 100000), "the last bytes are acked");
    TEST_ASSERT_FALSE(ota_ack_due(100000, 100000, 100000));
}

static void test_ring_wraps(void) {
    uint8_t buf[8];
    OtaRing r;
    ota_ring_init(&r, buf, sizeof(buf));
    const uint8_t a[] = {1, 2, 3, 4, 5, 6};
    ota_ring_push(&r, a, 6);
    TEST_ASSERT_EQUAL_UINT32(2, ota_ring_room(&r));
    uint8_t out[8];
    TEST_ASSERT_EQUAL_UINT32(4, ota_ring_pop(&r, out, 4));
    TEST_ASSERT_EQUAL_UINT8(4, out[3]);
    const uint8_t b[] = {7, 8, 9, 10, 11};
    ota_ring_push(&r, b, 5);   // wraps
    TEST_ASSERT_EQUAL_UINT32(7, ota_ring_used(&r));
    TEST_ASSERT_EQUAL_UINT32(7, ota_ring_pop(&r, out, 8));
    const uint8_t want[] = {5, 6, 7, 8, 9, 10, 11};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, out, 7);
    TEST_ASSERT_EQUAL_UINT32(11, r.tail);
}

static void test_replies(void) {
    uint8_t out[8];
    TEST_ASSERT_EQUAL(7, ota_reply_ready(out, 256, 16384));
    const uint8_t ready[] = {0x81, 0x00, 0x01, 0x00, 0x40, 0x00, 0x00};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ready, out, 7);
    TEST_ASSERT_EQUAL(5, ota_reply_offset(out, OTA_REPLY_AT, 0x01020304));
    const uint8_t at[] = {0x82, 0x04, 0x03, 0x02, 0x01};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(at, out, 5);
    TEST_ASSERT_EQUAL(1, ota_reply_done(out));
    TEST_ASSERT_EQUAL_UINT8(0x84, out[0]);
    TEST_ASSERT_EQUAL(2, ota_reply_error(out, OTA_ERR_CHECKSUM));
    TEST_ASSERT_EQUAL_UINT8(6, out[1]);
    TEST_ASSERT_EQUAL(2, ota_reply_confirm(out, OTA_CONFIRM_SECS));
    const uint8_t confirm[] = {0x86, 30};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(confirm, out, 2);
    TEST_ASSERT_EQUAL_UINT8(12, OTA_ERR_NOT_ALLOWED);
}

static void test_gate_belongs_to_one_connection(void) {
    OtaGate g;
    ota_gate_reset(&g);
    TEST_ASSERT_EQUAL(OTA_GATE_ASK, ota_gate_begin(&g, 1, 1000));
    TEST_ASSERT_EQUAL_MESSAGE(OTA_GATE_BUSY, ota_gate_begin(&g, 2, 2000), "another connection can't take it over");
    TEST_ASSERT_EQUAL(OTA_GATE_STILL_ASKING, ota_gate_begin(&g, 1, 20000));
    TEST_ASSERT_EQUAL_MESSAGE(19, ota_gate_left(&g, 12000), "a repeat BEGIN keeps the countdown");
    TEST_ASSERT_EQUAL(0, ota_gate_left(&g, 31000));
}

static void test_gate_presses_and_taps(void) {
    OtaGate g;
    ota_gate_reset(&g);
    bool allow;
    TEST_ASSERT_FALSE_MESSAGE(ota_gate_press(&g, 100, &allow), "no question: the press does its usual thing");
    ota_gate_begin(&g, 1, 1000);
    TEST_ASSERT_FALSE_MESSAGE(ota_gate_press(&g, 1500, &allow), "too soon: meant for something else");
    TEST_ASSERT_FALSE(ota_gate_tap(&g, 1500));
    TEST_ASSERT_TRUE(ota_gate_press(&g, 2500, &allow));
    TEST_ASSERT_FALSE_MESSAGE(allow, "one press isn't enough");
    TEST_ASSERT_TRUE(ota_gate_press(&g, 5000, &allow));
    TEST_ASSERT_FALSE_MESSAGE(allow, "two presses too far apart");
    TEST_ASSERT_TRUE(ota_gate_press(&g, 5800, &allow));
    TEST_ASSERT_TRUE(allow);
    TEST_ASSERT_TRUE(ota_gate_tap(&g, 2100));
}

static void test_gate_cools_down_after_no_answer(void) {
    OtaGate g;
    ota_gate_reset(&g);
    ota_gate_begin(&g, 1, 1000);
    ota_gate_end(&g, false, 31000);
    TEST_ASSERT_EQUAL(OTA_GATE_BUSY, ota_gate_begin(&g, 1, 40000));
    TEST_ASSERT_EQUAL(OTA_GATE_ASK, ota_gate_begin(&g, 1, 61000));
    ota_gate_end(&g, true, 62000);
    TEST_ASSERT_EQUAL_MESSAGE(OTA_GATE_ASK, ota_gate_begin(&g, 1, 63000), "no pause after an allowed one");
    // millis() wrapping during a cooldown
    ota_gate_reset(&g);
    ota_gate_begin(&g, 1, 0xFFFFFF00u);
    ota_gate_end(&g, false, 0xFFFFFF00u);
    TEST_ASSERT_EQUAL(OTA_GATE_BUSY, ota_gate_begin(&g, 1, 1000));
    TEST_ASSERT_EQUAL(OTA_GATE_ASK, ota_gate_begin(&g, 1, 40000));
}

// The whole exchange over a link that loses chunks: a host that keeps a
// window and rewinds on RESEND or silence (clawdmeter-ble's ota::Sender
// behaves the same) against the device's receive path and a slow flash.
static void test_lossy_link_delivers_the_image_intact(void) {
    const uint32_t size = 50000, chunk = 244, cap = 4096;
    std::vector<uint8_t> image(size), flash;
    for (uint32_t i = 0; i < size; i++) image[i] = (uint8_t)(i * 7 + (i >> 8));
    uint8_t buf[cap];
    OtaRing ring;
    ota_ring_init(&ring, buf, cap);
    OtaRx rx;
    ota_rx_start(&rx, size);

    uint32_t next = 0, acked = 0, last_ack = 0, sent = 0, rewinds = 0;
    unsigned seed = 1;
    for (int step = 0; step < 200000 && flash.size() < size; step++) {
        bool replied = false;
        // Host: send while the window allows; drop about one chunk in nine.
        if (next < size && next - acked + chunk <= cap) {
            const uint32_t n = next + chunk <= size ? chunk : size - next;
            seed = seed * 1103515245u + 12345u;
            const bool lost = (seed >> 16) % 9 == 0;
            sent++;
            if (!lost) {
                bool resend;
                if (ota_rx_chunk(&rx, next, n, ota_ring_room(&ring), &resend) == OTA_CHUNK_TAKE) {
                    ota_ring_push(&ring, &image[next], n);
                    ota_rx_took(&rx, n);
                }
                if (resend) {
                    next = rx.expected;   // RESEND(expected) reaches the host
                    rewinds++;
                    replied = true;
                    continue;
                }
            }
            next += n;
        }
        // Device loop: flash takes at most 300 bytes a step.
        uint8_t tmp[300];
        const uint32_t got = ota_ring_pop(&ring, tmp, sizeof(tmp));
        flash.insert(flash.end(), tmp, tmp + got);
        if (ota_ack_due((uint32_t)flash.size(), last_ack, size)) {
            last_ack = (uint32_t)flash.size();
            acked = last_ack;   // AT reaches the host
            if (next < acked) next = acked;
            replied = true;
        }
        // Host: the window is full and nothing came back: rewind.
        if (!replied && got == 0 && next - acked + chunk > cap && next > rx.expected) {
            next = acked;
            rewinds++;
        }
        // The tail chunk was lost and nothing follows it to show a gap.
        if (!replied && got == 0 && next >= size && acked < size) {
            next = acked;
            rewinds++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(size, flash.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(image.data(), flash.data(), size);
    TEST_ASSERT_TRUE_MESSAGE(rewinds > 0, "the drops were exercised");
    TEST_ASSERT_TRUE_MESSAGE(sent < size / chunk * 2, "resends stay a fraction of the image");
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_begin_parses_and_checks);
    RUN_TEST(test_begin_rejects_bad_frames);
    RUN_TEST(test_data_frames);
    RUN_TEST(test_chunks_in_order_gap_and_duplicate);
    RUN_TEST(test_full_buffer_asks_to_resend);
    RUN_TEST(test_ack_cadence);
    RUN_TEST(test_ring_wraps);
    RUN_TEST(test_replies);
    RUN_TEST(test_lossy_link_delivers_the_image_intact);
    RUN_TEST(test_gate_belongs_to_one_connection);
    RUN_TEST(test_gate_presses_and_taps);
    RUN_TEST(test_gate_cools_down_after_no_answer);
    return UNITY_END();
}
