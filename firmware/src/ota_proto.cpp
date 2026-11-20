#include "ota_proto.h"

#include <string.h>

static uint32_t get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

bool ota_parse_begin(const uint8_t* frame, size_t len, OtaBegin* out) {
    if (len < OTA_BEGIN_MIN || frame[0] != OTA_OP_BEGIN) return false;
    const size_t env_len = len - (1 + 4 + OTA_SHA_LEN);
    if (env_len > OTA_ENV_MAX) return false;
    const uint8_t* env = frame + 1 + 4 + OTA_SHA_LEN;
    for (size_t i = 0; i < env_len; i++) {
        if (env[i] < 0x21 || env[i] > 0x7E) return false;
    }
    out->size = get_u32(frame + 1);
    memcpy(out->sha, frame + 5, OTA_SHA_LEN);
    memcpy(out->env, env, env_len);
    out->env[env_len] = '\0';
    return true;
}

OtaErr ota_check_begin(const OtaBegin* b, const char* env, uint32_t slot_size) {
    if (slot_size == 0) return OTA_ERR_NO_SLOT;
    if (strcmp(b->env, env) != 0) return OTA_ERR_WRONG_BOARD;
    if (b->size == 0 || b->size > slot_size) return OTA_ERR_TOO_BIG;
    return OTA_ERR_NONE;
}

bool ota_parse_data(const uint8_t* frame, size_t len, uint32_t* offset, const uint8_t** payload, size_t* n) {
    if (len <= OTA_DATA_HEADER || frame[0] != OTA_OP_DATA) return false;
    *offset = get_u32(frame + 1);
    *payload = frame + OTA_DATA_HEADER;
    *n = len - OTA_DATA_HEADER;
    return true;
}

void ota_rx_start(OtaRx* rx, uint32_t size) {
    rx->size = size;
    rx->expected = 0;
    rx->resend_for = OTA_NO_RESEND;
}

OtaChunk ota_rx_chunk(OtaRx* rx, uint32_t offset, size_t n, size_t room, bool* resend) {
    *resend = false;
    if ((uint64_t)offset + n > rx->size) return OTA_CHUNK_OVERRUN;
    if (offset < rx->expected) return OTA_CHUNK_DUPLICATE;
    OtaChunk c;
    if (offset > rx->expected) c = OTA_CHUNK_GAP;
    else if (n > room) c = OTA_CHUNK_FULL;
    else return OTA_CHUNK_TAKE;
    if (rx->resend_for != rx->expected) {
        rx->resend_for = rx->expected;
        *resend = true;
    }
    return c;
}

void ota_rx_took(OtaRx* rx, size_t n) {
    rx->expected += (uint32_t)n;
    rx->resend_for = OTA_NO_RESEND;
}

bool ota_ack_due(uint32_t written, uint32_t last_acked, uint32_t size) {
    if (written <= last_acked) return false;
    return written >= size || written - last_acked >= OTA_ACK_STEP;
}

void ota_ring_init(OtaRing* r, uint8_t* buf, uint32_t cap) {
    r->buf = buf;
    r->cap = cap;
    r->head = 0;
    r->tail = 0;
}

uint32_t ota_ring_used(const OtaRing* r) { return r->head - r->tail; }

uint32_t ota_ring_room(const OtaRing* r) { return r->cap - ota_ring_used(r); }

void ota_ring_push(OtaRing* r, const uint8_t* src, uint32_t n) {
    uint32_t at = r->head % r->cap;
    const uint32_t first = n < r->cap - at ? n : r->cap - at;
    memcpy(r->buf + at, src, first);
    memcpy(r->buf, src + first, n - first);
    r->head += n;
}

uint32_t ota_ring_pop(OtaRing* r, uint8_t* dst, uint32_t max) {
    uint32_t n = ota_ring_used(r);
    if (n > max) n = max;
    const uint32_t at = r->tail % r->cap;
    const uint32_t first = n < r->cap - at ? n : r->cap - at;
    memcpy(dst, r->buf + at, first);
    memcpy(dst + first, r->buf, n - first);
    r->tail += n;
    return n;
}

void ota_gate_reset(OtaGate* g) { memset(g, 0, sizeof(*g)); }

OtaGateBegin ota_gate_begin(OtaGate* g, uint16_t conn, uint32_t now) {
    if (g->asking) return conn == g->conn ? OTA_GATE_STILL_ASKING : OTA_GATE_BUSY;
    if (g->cooling && (int32_t)(g->cool_until - now) > 0) return OTA_GATE_BUSY;
    g->cooling = false;
    g->asking = true;
    g->conn = conn;
    g->asked_at = now;
    g->has_first = false;
    return OTA_GATE_ASK;
}

bool ota_gate_press(OtaGate* g, uint32_t now, bool* allow) {
    *allow = false;
    if (!g->asking || now - g->asked_at < OTA_SETTLE_MS) return false;
    if (g->has_first && now - g->first_at <= OTA_DOUBLE_PRESS_MS) {
        *allow = true;
        g->has_first = false;
    } else {
        g->has_first = true;
        g->first_at = now;
    }
    return true;
}

bool ota_gate_tap(const OtaGate* g, uint32_t now) {
    return g->asking && now - g->asked_at >= OTA_SETTLE_MS;
}

int ota_gate_left(const OtaGate* g, uint32_t now) {
    const uint32_t waited = now - g->asked_at;
    if (waited >= (uint32_t)OTA_CONFIRM_SECS * 1000) return 0;
    return OTA_CONFIRM_SECS - (int)(waited / 1000);
}

void ota_gate_end(OtaGate* g, bool allowed, uint32_t now) {
    g->asking = false;
    g->has_first = false;
    if (!allowed) {
        g->cooling = true;
        g->cool_until = now + OTA_COOLDOWN_MS;
    }
}

size_t ota_reply_ready(uint8_t* out, uint16_t mtu, uint32_t window) {
    out[0] = OTA_REPLY_READY;
    out[1] = (uint8_t)mtu;
    out[2] = (uint8_t)(mtu >> 8);
    put_u32(out + 3, window);
    return 7;
}

size_t ota_reply_offset(uint8_t* out, OtaReply kind, uint32_t offset) {
    out[0] = kind;
    put_u32(out + 1, offset);
    return 5;
}

size_t ota_reply_done(uint8_t* out) {
    out[0] = OTA_REPLY_DONE;
    return 1;
}

size_t ota_reply_confirm(uint8_t* out, uint8_t seconds) {
    out[0] = OTA_REPLY_CONFIRM;
    out[1] = seconds;
    return 2;
}

size_t ota_reply_error(uint8_t* out, OtaErr code) {
    out[0] = OTA_REPLY_ERROR;
    out[1] = code;
    return 2;
}
