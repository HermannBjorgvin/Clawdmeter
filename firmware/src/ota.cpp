#include "ota.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <lvgl.h>
#include <mbedtls/sha256.h>
#include <string.h>

#include "ble.h"
#include "build_info.h"
#include "idle.h"
#include "ota_proto.h"
#include "ota_ui.h"

// Arduino marks a freshly installed image as good during boot unless this
// says to wait (initArduino in esp32-hal-misc.c). Waiting leaves the image
// pending until ota_tick() confirms it, after the owner's computer has
// reconnected over it; the bootloader rolls back an image that resets before
// then. A USB flash leaves no pending state, so this changes nothing there.
extern "C" bool verifyRollbackLater(void) { return true; }

// Receive buffer between the host task and flash: PSRAM where there is
// some, else a smaller one from internal RAM. Its size is the window the
// app may have in flight.
static const uint32_t RING_PSRAM = 16384;
static const uint32_t RING_INTERNAL = 8192;
// No data for this long while receiving: the app is gone, drop the update.
static const uint32_t STALL_MS = 30000;
// After DONE: let the reply and the write response go out, then drop the
// links (so the app sees the restart at once), then restart.
static const uint32_t DISCONNECT_AFTER_MS = 600;
static const uint32_t RESTART_AFTER_MS = 1200;
// How long "Update stopped" stays up.
static const uint32_t FAILED_SHOW_MS = 4000;
// A pending image is confirmed this long after the owner connected, even
// with no payload yet (the app may not be running).
static const uint32_t CONFIRM_AFTER_MS = 60000;
// A duplicate chunk with nothing left to write repeats the last AT at most
// this often (the AT for the last bytes may have been lost).
static const uint32_t REACK_MS = 500;
// Flash work per loop pass, so the screen keeps moving.
static const uint32_t DRAIN_BUDGET = 8192;
static const uint16_t NO_CONN = 0xFFFF;

enum State : uint8_t { IDLE, CONFIRMING, RECEIVING, RESTARTING };

// Shared with the host task: everything below `lock` up to the loop-only
// section. The host task only reads `state` and touches the ring and `rx`
// under the lock.
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static volatile State state = IDLE;
static uint16_t ota_conn = NO_CONN;
static OtaRing ring;
static OtaRx rx;
static uint32_t last_data_ms = 0;
static uint32_t flashed = 0;        // bytes written to flash (the loop's `written`)
static uint32_t reack_ms = 0;
// Requests the loop takes.
static bool req_begin = false, req_end = false, req_drop = false, req_abort = false;
static uint16_t drop_conn = NO_CONN;   // the connection req_drop is for
static OtaBegin pending;
static uint16_t pending_conn = NO_CONN, pending_mtu = 23;
static volatile bool owner_link = false;

// Loop task only.
static uint32_t slot_size = 0;
static bool pending_verify = false;
static uint8_t* ring_buf = nullptr;
static const esp_partition_t* target = nullptr;
static esp_ota_handle_t handle = 0;
static bool handle_open = false;
static mbedtls_sha256_context sha;
static uint8_t want_sha[OTA_SHA_LEN];
static uint32_t written = 0, last_ack = 0, image_size = 0;
static uint32_t restart_at = 0;
static uint32_t failed_at = 0;
static bool failed_shown = false;
static uint32_t awake_at = 0;
static bool owner_seen = false;     // the loop has seen the owner's link
static uint32_t owner_seen_at = 0;  // when it first did
static bool payload_ok = false;
// The update waiting to be allowed on the board (CONFIRMING), and who may
// ask, the countdown, presses and the pause after an unanswered question.
static OtaBegin asked;
static uint16_t asked_conn = NO_CONN, asked_mtu = 23;
static bool allowed = false;
static OtaGate gate;

static void reply(const uint8_t* data, size_t len, uint16_t conn) {
    ble_ota_reply(data, len, conn);
}

static void reply_error(OtaErr code, uint16_t conn) {
    uint8_t out[8];
    reply(out, ota_reply_error(out, code), conn);
}

void ota_init(void) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    // A layout with one app slot hands back the running one: no updates.
    slot_size = (running && next && next != running) ? next->size : 0;
    esp_ota_img_states_t st;
    if (running && esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        pending_verify = true;
        Serial.printf("OTA: new firmware in %s, kept once the owner's computer reconnects and a payload parses\n",
                      running->label);
    }
    Serial.printf("OTA: running from %s, update slot %lu bytes\n", running ? running->label : "?",
                  (unsigned long)slot_size);
}

uint32_t ota_slot_size(void) { return slot_size; }

bool ota_active(void) { return state != IDLE; }

bool ota_allow(void) {
    if (state != CONFIRMING) return false;
    allowed = true;
    return true;
}

bool ota_button_press(void) {
    if (state != CONFIRMING) return false;
    bool allow = false;
    const bool used = ota_gate_press(&gate, millis(), &allow);
    if (allow) allowed = true;
    return used;
}

void ota_on_owner_link(void) { owner_link = true; }

void ota_on_payload(void) { payload_ok = true; }

void ota_on_disconnect(uint16_t conn) {
    portENTER_CRITICAL(&lock);
    // Whatever the state: the loop may have taken this connection's BEGIN
    // and not shown its question yet. A drop of the update's own link wins
    // over another one in the same pass.
    if (!req_drop || conn == ota_conn) {
        req_drop = true;
        drop_conn = conn;
    }
    // A BEGIN still waiting for the loop came over the link that's gone.
    if (req_begin && pending_conn == conn) req_begin = false;
    portEXIT_CRITICAL(&lock);
}

void ota_on_frame(const uint8_t* data, size_t len, uint16_t conn, uint16_t mtu) {
    if (len == 0) return;
    switch (data[0]) {
    case OTA_OP_DATA: {
        uint32_t offset;
        const uint8_t* payload;
        size_t n;
        if (!ota_parse_data(data, len, &offset, &payload, &n)) return;
        bool resend = false, reack = false;
        uint32_t expected = 0;
        portENTER_CRITICAL(&lock);
        if (state == RECEIVING && conn == ota_conn) {
            const OtaChunk c = ota_rx_chunk(&rx, offset, n, ota_ring_room(&ring), &resend);
            if (c == OTA_CHUNK_TAKE) {
                ota_ring_push(&ring, payload, (uint32_t)n);
                ota_rx_took(&rx, n);
            } else if (c == OTA_CHUNK_DUPLICATE && flashed == rx.expected && millis() - reack_ms >= REACK_MS) {
                // The app is resending what's already written: its AT went
                // missing. Say it again.
                reack = true;
                reack_ms = millis();
            }
            expected = rx.expected;
            last_data_ms = millis();
        }
        portEXIT_CRITICAL(&lock);
        if (resend || reack) {
            uint8_t out[8];
            reply(out, ota_reply_offset(out, resend ? OTA_REPLY_RESEND : OTA_REPLY_AT, expected), conn);
        }
        return;
    }
    case OTA_OP_BEGIN: {
        OtaBegin b;
        if (!ota_parse_begin(data, len, &b)) {
            reply_error(OTA_ERR_BAD_FRAME, conn);
            return;
        }
        portENTER_CRITICAL(&lock);
        pending = b;
        pending_conn = conn;
        pending_mtu = mtu;
        req_begin = true;
        portEXIT_CRITICAL(&lock);
        return;
    }
    case OTA_OP_END:
        portENTER_CRITICAL(&lock);
        req_end = true;
        pending_conn = conn;
        portEXIT_CRITICAL(&lock);
        return;
    case OTA_OP_ABORT:
        portENTER_CRITICAL(&lock);
        req_abort = true;
        // A BEGIN the loop hasn't taken yet is cancelled too (the loop
        // handles ABORT before BEGIN, so it would start after it).
        req_begin = false;
        portEXIT_CRITICAL(&lock);
        return;
    default:
        reply_error(OTA_ERR_BAD_FRAME, conn);
        return;
    }
}

static void show_failed(void) {
    ota_ui_show(OTA_UI_FAILED, 0);
    failed_at = millis();
    failed_shown = true;
}

// Stop receiving and free everything; `code` (when not NONE) goes to the app.
static void stop(OtaErr code, const char* why) {
    const uint16_t conn = ota_conn;
    portENTER_CRITICAL(&lock);
    state = IDLE;
    ota_conn = NO_CONN;
    ring.buf = nullptr;
    portEXIT_CRITICAL(&lock);
    if (handle_open) esp_ota_abort(handle);
    handle_open = false;
    mbedtls_sha256_free(&sha);
    heap_caps_free(ring_buf);
    ring_buf = nullptr;
    Serial.printf("OTA: stopped at %lu of %lu bytes: %s\n", (unsigned long)written, (unsigned long)image_size, why);
    if (code != OTA_ERR_NONE) reply_error(code, conn);
    show_failed();
}

// A valid BEGIN the gate let through: ask on the board before anything is
// written.
static void ask(const OtaBegin& b, uint16_t conn, uint16_t mtu) {
    asked = b;
    asked_conn = conn;
    asked_mtu = mtu;
    allowed = false;
    (void)ota_ui_take_tap();   // a tap left over from before doesn't count
    failed_shown = false;
    portENTER_CRITICAL(&lock);
    ota_conn = conn;
    state = CONFIRMING;
    portEXIT_CRITICAL(&lock);
    Serial.printf("OTA: %lu bytes for %s; waiting %d s for a tap or button press to allow it\n",
                  (unsigned long)b.size, b.env, OTA_CONFIRM_SECS);
    idle_note_activity();
    awake_at = millis();
    ota_ui_show(OTA_UI_CONFIRM, OTA_CONFIRM_SECS);
    uint8_t out[8];
    reply(out, ota_reply_confirm(out, OTA_CONFIRM_SECS), conn);
}

// The update waiting to be allowed won't be: refused in time (`code`), or
// dropped by the app or the link (NONE).
static void unask(OtaErr code, const char* why) {
    ota_gate_end(&gate, false, millis());
    portENTER_CRITICAL(&lock);
    state = IDLE;
    ota_conn = NO_CONN;
    portEXIT_CRITICAL(&lock);
    Serial.printf("OTA: not allowed: %s\n", why);
    if (code != OTA_ERR_NONE) reply_error(code, asked_conn);
    show_failed();
}

// Allowed: set up the write. An error comes back for the caller to send.
static OtaErr start(const OtaBegin& b, uint16_t conn, uint16_t mtu) {
    uint32_t cap = RING_PSRAM;
    ring_buf = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ring_buf) {
        cap = RING_INTERNAL;
        ring_buf = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!ring_buf) return OTA_ERR_NO_MEMORY;
    target = esp_ota_get_next_update_partition(nullptr);
    // Sequential writes erase each sector as the data reaches it, a few tens
    // of milliseconds at a time, instead of the whole image up front.
    const esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        heap_caps_free(ring_buf);
        ring_buf = nullptr;
        Serial.printf("OTA: esp_ota_begin failed: %s\n", esp_err_to_name(err));
        return OTA_ERR_FLASH;
    }
    handle_open = true;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    memcpy(want_sha, b.sha, sizeof(want_sha));
    image_size = b.size;
    written = last_ack = 0;
    flashed = 0;
    failed_shown = false;
    portENTER_CRITICAL(&lock);
    ota_ring_init(&ring, ring_buf, cap);
    ota_rx_start(&rx, b.size);
    ota_conn = conn;
    last_data_ms = millis();
    state = RECEIVING;
    portEXIT_CRITICAL(&lock);
    Serial.printf("OTA: receiving %lu bytes into %s (mtu %u, window %lu)\n", (unsigned long)b.size, target->label,
                  mtu, (unsigned long)cap);
    idle_note_activity();
    awake_at = millis();
    ota_ui_show(OTA_UI_RECEIVING, 0);
    uint8_t out[8];
    reply(out, ota_reply_ready(out, mtu, cap), conn);
    return OTA_ERR_NONE;
}

// Move what has arrived into flash; AT as it goes.
static void drain(void) {
    uint8_t stage[1024];
    uint32_t budget = DRAIN_BUDGET;
    while (budget > 0 && state == RECEIVING) {
        portENTER_CRITICAL(&lock);
        const uint32_t n = ota_ring_pop(&ring, stage, sizeof(stage));
        portEXIT_CRITICAL(&lock);
        if (n == 0) return;
        const esp_err_t err = esp_ota_write(handle, stage, n);
        if (err != ESP_OK) {
            Serial.printf("OTA: esp_ota_write failed: %s\n", esp_err_to_name(err));
            stop(err == ESP_ERR_OTA_VALIDATE_FAILED ? OTA_ERR_INVALID : OTA_ERR_FLASH, "write failed");
            return;
        }
        mbedtls_sha256_update(&sha, stage, n);
        written += n;
        portENTER_CRITICAL(&lock);
        flashed = written;
        portEXIT_CRITICAL(&lock);
        budget = budget > n ? budget - n : 0;
        if (ota_ack_due(written, last_ack, image_size)) {
            last_ack = written;
            uint8_t out[8];
            reply(out, ota_reply_offset(out, OTA_REPLY_AT, written), ota_conn);
        }
    }
}

static void finish(uint16_t conn) {
    if (written != image_size) {
        stop(OTA_ERR_INCOMPLETE, "END before every byte arrived");
        return;
    }
    ota_ui_show(OTA_UI_VERIFYING, 100);
    lv_refr_now(nullptr);
    uint8_t got[OTA_SHA_LEN];
    mbedtls_sha256_finish(&sha, got);
    if (memcmp(got, want_sha, sizeof(got)) != 0) {
        stop(OTA_ERR_CHECKSUM, "checksum mismatch");
        return;
    }
    // esp_ota_end checks the image the way the bootloader will, and frees
    // the handle either way.
    esp_err_t err = esp_ota_end(handle);
    handle_open = false;
    if (err != ESP_OK) {
        Serial.printf("OTA: esp_ota_end failed: %s\n", esp_err_to_name(err));
        stop(err == ESP_ERR_OTA_VALIDATE_FAILED ? OTA_ERR_INVALID : OTA_ERR_FLASH, "image rejected");
        return;
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        Serial.printf("OTA: esp_ota_set_boot_partition failed: %s\n", esp_err_to_name(err));
        stop(OTA_ERR_FLASH, "couldn't switch the boot slot");
        return;
    }
    mbedtls_sha256_free(&sha);
    portENTER_CRITICAL(&lock);
    state = RESTARTING;
    ring.buf = nullptr;
    portEXIT_CRITICAL(&lock);
    heap_caps_free(ring_buf);
    ring_buf = nullptr;
    Serial.printf("OTA: %lu bytes verified, restarting into %s\n", (unsigned long)written, target->label);
    uint8_t out[8];
    reply(out, ota_reply_done(out), conn);
    // No new link to the old firmware in the second before the restart: the
    // app would read the old version and take the update for a rollback.
    ble_hold_advertising();
    ota_ui_show(OTA_UI_RESTARTING, 100);
    restart_at = millis();
}

static void confirm(const char* why) {
    pending_verify = false;
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    Serial.printf("OTA: %s, firmware confirmed (%s)\n", why, esp_err_to_name(err));
}

void ota_tick(void) {
    // The owner reached us over this image, and the app's first payload
    // went through (or a minute passed without one): keep it. Done here, not
    // in the BLE callback, so the loop has run too; waiting for a payload
    // means an image that crashes on one still goes back.
    if (pending_verify && owner_link) {
        if (!owner_seen) {
            owner_seen = true;
            owner_seen_at = millis();
        }
        if (payload_ok) confirm("owner reconnected and a payload parsed");
        else if (millis() - owner_seen_at >= CONFIRM_AFTER_MS) confirm("owner connected for a minute");
    }

    portENTER_CRITICAL(&lock);
    // A drop counts only for the connection the update (or its question) is
    // on, checked again after a BEGIN in this pass asks.
    const bool begin = req_begin, end = req_end, abort_req = req_abort;
    const uint16_t dropped = req_drop ? drop_conn : NO_CONN;
    const bool drop = dropped != NO_CONN && dropped == ota_conn;
    const OtaBegin b = pending;
    const uint16_t conn = pending_conn, mtu = pending_mtu;
    req_begin = req_end = req_drop = req_abort = false;
    portEXIT_CRITICAL(&lock);

    if (state == RESTARTING) {
        const uint32_t since = millis() - restart_at;
        static bool dropped = false;
        if (!dropped && since >= DISCONNECT_AFTER_MS) {
            dropped = true;
            ble_disconnect_all();
        }
        if (since >= RESTART_AFTER_MS) esp_restart();
        return;
    }
    if (state == RECEIVING && (drop || abort_req)) stop(OTA_ERR_NONE, drop ? "link lost" : "cancelled by the app");
    if (state == CONFIRMING && (drop || abort_req)) unask(OTA_ERR_NONE, drop ? "link lost" : "cancelled by the app");
    if (begin) {
        const OtaErr check = ota_check_begin(&b, CLAWD_ENV, slot_size);
        if (check != OTA_ERR_NONE) {
            Serial.printf("OTA: refused %lu bytes for %s (error %d)\n", (unsigned long)b.size, b.env, (int)check);
            reply_error(check, conn);
        } else if (state == RECEIVING && conn != ota_conn) {
            // Another connection can't cut into a running update.
            reply_error(OTA_ERR_BUSY, conn);
        } else {
            // The same connection starting over (the app lost track): ask
            // again, from scratch.
            if (state == RECEIVING) stop(OTA_ERR_NONE, "restarted by the app");
            switch (ota_gate_begin(&gate, conn, millis())) {
            case OTA_GATE_ASK:
                ask(b, conn, mtu);
                break;
            case OTA_GATE_STILL_ASKING: {
                // Its question is up already: keep the countdown.
                asked = b;
                asked_mtu = mtu;
                uint8_t out[8];
                reply(out, ota_reply_confirm(out, (uint8_t)ota_gate_left(&gate, millis())), conn);
                break;
            }
            case OTA_GATE_BUSY:
                Serial.println("OTA: refused a BEGIN: another connection's question, or a cooldown");
                reply_error(OTA_ERR_BUSY, conn);
                break;
            }
            // Its link went in this same pass.
            if (state == CONFIRMING && dropped != NO_CONN && dropped == ota_conn) unask(OTA_ERR_NONE, "link lost");
        }
    }
    if (state == CONFIRMING) {
        const uint32_t now = millis();
        // A tap counts once the question has settled; one before is dropped.
        if (ota_ui_take_tap() && ota_gate_tap(&gate, now)) allowed = true;
        if (allowed) {
            allowed = false;
            ota_gate_end(&gate, true, now);
            // ESP-IDF won't write the other slot while this image is
            // unconfirmed; someone allowing an update on it is here and the
            // owner's link works.
            if (pending_verify) confirm("an update was allowed on the board");
            Serial.println("OTA: allowed on the board");
            const OtaErr err = start(asked, asked_conn, asked_mtu);
            if (err != OTA_ERR_NONE) {
                portENTER_CRITICAL(&lock);
                state = IDLE;
                ota_conn = NO_CONN;
                portEXIT_CRITICAL(&lock);
                reply_error(err, asked_conn);
                show_failed();
            }
        } else if (ota_gate_left(&gate, now) == 0) {
            unask(OTA_ERR_NOT_ALLOWED, "nobody allowed it in time");
        } else {
            ota_ui_show(OTA_UI_CONFIRM, ota_gate_left(&gate, now));
            if (millis() - awake_at >= 10000) {
                awake_at = millis();
                idle_note_activity();
            }
        }
    }
    if (state == RECEIVING) {
        drain();
        portENTER_CRITICAL(&lock);
        const uint32_t quiet = millis() - last_data_ms;
        portEXIT_CRITICAL(&lock);
        // No data and no END for this long: the app is gone.
        if (state == RECEIVING && !end && quiet >= STALL_MS) stop(OTA_ERR_TIMEOUT, "no data");
    }
    if (end) {
        if (state == RECEIVING) {
            finish(conn);
        } else if (state == IDLE) {
            reply_error(OTA_ERR_NOT_STARTED, conn);
        }
    }
    if (state == RECEIVING) {
        ota_ui_show(OTA_UI_RECEIVING, image_size ? (int)((uint64_t)written * 100 / image_size) : 0);
        // Keep the panel lit while it updates.
        if (millis() - awake_at >= 10000) {
            awake_at = millis();
            idle_note_activity();
        }
    } else if (failed_shown && millis() - failed_at >= FAILED_SHOW_MS) {
        failed_shown = false;
        ota_ui_hide();
    }
}
