#pragma once
// Firmware updates over Bluetooth: the wire format and the receive logic,
// pure (no Arduino, no ESP-IDF) so the native tests cover it:
//   pio test -d firmware -e native -f test_ota
// ota.cpp does the flash side. The protocol is in docs/protocol.md.
//
// Host → device frames go to the OTA characteristic (little-endian):
//   BEGIN  0x01 | u32 size | 32-byte SHA-256 of the image | env (ASCII, no NUL)
//   DATA   0x02 | u32 offset | payload            (write without response)
//   END    0x03
//   ABORT  0x04
// Device → host replies are notified on TX. Their first byte is 0x80 or
// above, so they never read as the JSON acks TX also carries:
//   CONFIRM 0x86 | u8 seconds                (BEGIN is valid; waiting for a tap or
//                                            button press on the board to allow it)
//   READY  0x81 | u16 ATT MTU | u32 window   (allowed; send data)
//   AT     0x82 | u32 bytes written to flash
//   RESEND 0x83 | u32 offset                 (a chunk went missing; resend from here)
//   DONE   0x84                              (verified, boot slot switched, restarting)
//   ERROR  0x85 | u8 code
#include <stddef.h>
#include <stdint.h>

// Bumped when the frames above change incompatibly; the INFO
// characteristic reports it as `otav`.
#define OTA_PROTO_VERSION 1

#define OTA_SHA_LEN     32
#define OTA_ENV_MAX     40
#define OTA_DATA_HEADER 5   // op + u32 offset
#define OTA_BEGIN_MIN   (1 + 4 + OTA_SHA_LEN + 1)
// AT goes out at least this often while data flows, and once at the end.
#define OTA_ACK_STEP    2048u
// How long the board waits for someone to allow an update on it.
#define OTA_CONFIRM_SECS 30
// Taps and presses this soon after the question appears don't count (they
// were meant for something else).
#define OTA_SETTLE_MS 1000
// A button allows it with two presses this close together.
#define OTA_DOUBLE_PRESS_MS 1500
// After a question ends unanswered (or cancelled), no new one for this long.
#define OTA_COOLDOWN_MS 30000

enum OtaOp : uint8_t {
    OTA_OP_BEGIN = 0x01,
    OTA_OP_DATA  = 0x02,
    OTA_OP_END   = 0x03,
    OTA_OP_ABORT = 0x04,
};

enum OtaReply : uint8_t {
    OTA_REPLY_READY  = 0x81,
    OTA_REPLY_AT     = 0x82,
    OTA_REPLY_RESEND = 0x83,
    OTA_REPLY_DONE   = 0x84,
    OTA_REPLY_ERROR  = 0x85,
    OTA_REPLY_CONFIRM = 0x86,
};

// ERROR codes. The app maps them to messages (clawdmeter-ble's ota.rs).
enum OtaErr : uint8_t {
    OTA_ERR_NONE        = 0,
    OTA_ERR_NO_SLOT     = 1,   // this board has no second app slot: USB only
    OTA_ERR_TOO_BIG     = 2,   // the image is larger than the slot (or empty)
    OTA_ERR_WRONG_BOARD = 3,   // BEGIN named another env
    OTA_ERR_BAD_FRAME   = 4,   // a frame too short or of an unknown op
    OTA_ERR_FLASH       = 5,   // starting, writing or switching the slot failed
    OTA_ERR_CHECKSUM    = 6,   // the bytes received don't match BEGIN's SHA-256
    OTA_ERR_INVALID     = 7,   // the image failed the bootloader's own checks
    OTA_ERR_NOT_STARTED = 8,   // END with no update running
    OTA_ERR_INCOMPLETE  = 9,   // END before every byte arrived
    OTA_ERR_NO_MEMORY   = 10,  // no room for the receive buffer
    OTA_ERR_TIMEOUT     = 11,  // no data for too long; the update was dropped
    OTA_ERR_NOT_ALLOWED = 12,  // nobody allowed it on the board in time
    OTA_ERR_BUSY        = 13,  // another connection's update, or one refused moments ago
};

struct OtaBegin {
    uint32_t size;
    uint8_t  sha[OTA_SHA_LEN];
    char     env[OTA_ENV_MAX + 1];
};

// A BEGIN frame (op byte included). False when it's too short or its env
// is empty, too long or not printable ASCII.
bool ota_parse_begin(const uint8_t* frame, size_t len, OtaBegin* out);

// Whether this board takes the update: OTA_ERR_NONE, or why not.
// `slot_size` 0 = no second app slot.
OtaErr ota_check_begin(const OtaBegin* b, const char* env, uint32_t slot_size);

// A DATA frame (op byte included): its offset and payload. False when it's
// shorter than the header or carries no payload.
bool ota_parse_data(const uint8_t* frame, size_t len, uint32_t* offset, const uint8_t** payload, size_t* n);

// Where received chunks stand. `expected` is the next offset the receive
// buffer takes; everything before it has arrived in order.
struct OtaRx {
    uint32_t size;
    uint32_t expected;
    uint32_t resend_for;   // the `expected` a RESEND went out for, or OTA_NO_RESEND
};
#define OTA_NO_RESEND 0xFFFFFFFFu

void ota_rx_start(OtaRx* rx, uint32_t size);

enum OtaChunk {
    OTA_CHUNK_TAKE,       // in order: append it, then ota_rx_took()
    OTA_CHUNK_DUPLICATE,  // already have it (a resend after a rewind): drop
    OTA_CHUNK_GAP,        // something before it went missing: drop
    OTA_CHUNK_FULL,       // in order, but the buffer has no room: drop
    OTA_CHUNK_OVERRUN,    // runs past the image size: drop
};

// What to do with `n` bytes at `offset` while the buffer has `room` bytes
// free. `*resend` is set when a RESEND(expected) should go out: on a gap or
// a full buffer, once per expected offset, so a burst of chunks behind one
// lost chunk asks once.
OtaChunk ota_rx_chunk(OtaRx* rx, uint32_t offset, size_t n, size_t room, bool* resend);
// After a TAKE: the chunk is in, expect the next.
void ota_rx_took(OtaRx* rx, size_t n);

// AT is due: another OTA_ACK_STEP written since the last one, or the end.
bool ota_ack_due(uint32_t written, uint32_t last_acked, uint32_t size);

// Single-producer, single-consumer byte ring. `head` and `tail` count bytes
// pushed and popped since the start (they are image offsets). Callers lock.
struct OtaRing {
    uint8_t* buf;
    uint32_t cap;
    uint32_t head;
    uint32_t tail;
};

void     ota_ring_init(OtaRing* r, uint8_t* buf, uint32_t cap);
uint32_t ota_ring_used(const OtaRing* r);
uint32_t ota_ring_room(const OtaRing* r);
// Appends all `n` bytes; the caller checked the room.
void     ota_ring_push(OtaRing* r, const uint8_t* src, uint32_t n);
// Takes up to `max` bytes; returns how many.
uint32_t ota_ring_pop(OtaRing* r, uint8_t* dst, uint32_t max);

// The on-board approval: who asked, the countdown, presses and taps, and
// the pause after a question nobody answered. Loop task only.
struct OtaGate {
    bool     asking;       // the question is on screen
    uint16_t conn;         // the connection it's for
    uint32_t asked_at;
    bool     has_first;    // a first press of a double press
    uint32_t first_at;
    bool     cooling;      // no new question until cool_until
    uint32_t cool_until;
};

enum OtaGateBegin {
    OTA_GATE_ASK,           // show the question for this connection
    OTA_GATE_STILL_ASKING,  // this connection's question is already up: keep its countdown
    OTA_GATE_BUSY,          // another connection's question is up, or it's cooling down
};

void ota_gate_reset(OtaGate* g);
OtaGateBegin ota_gate_begin(OtaGate* g, uint16_t conn, uint32_t now);
// A button press. True when the question takes it (up and settled): it does
// nothing else then. `*allow` is set on the second press in time.
bool ota_gate_press(OtaGate* g, uint32_t now, bool* allow);
// A tap on the question: true when it allows it (settled).
bool ota_gate_tap(const OtaGate* g, uint32_t now);
// Seconds left to answer; 0 = time is up.
int  ota_gate_left(const OtaGate* g, uint32_t now);
// The question is gone: allowed, or not (then a cooldown).
void ota_gate_end(OtaGate* g, bool allowed, uint32_t now);

// Replies, written to `out` (at least 8 bytes); return their length.
size_t ota_reply_ready(uint8_t* out, uint16_t mtu, uint32_t window);
size_t ota_reply_offset(uint8_t* out, OtaReply kind, uint32_t offset);   // AT, RESEND
size_t ota_reply_done(uint8_t* out);
size_t ota_reply_confirm(uint8_t* out, uint8_t seconds);
size_t ota_reply_error(uint8_t* out, OtaErr code);
