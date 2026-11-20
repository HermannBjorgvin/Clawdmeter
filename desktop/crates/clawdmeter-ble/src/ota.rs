//! Firmware updates over Bluetooth: the frames the app writes, the replies
//! the board notifies, what its INFO characteristic says, and the sending
//! side's window. The board's half is `firmware/src/ota_proto.h`; the
//! exchange is described in `docs/protocol.md` ("Firmware updates").
//!
//! The app writes BEGIN (size, SHA-256, env); the board answers CONFIRM and
//! asks on its screen, and only once someone taps it or presses a button does
//! it send READY (its ATT MTU and receive window). The app then streams DATA frames without response while
//! fewer than a window's bytes are unacknowledged, moves on as AT replies
//! report bytes written to flash, rewinds on RESEND or after a quiet spell,
//! and writes END once every byte is acknowledged. DONE means the board
//! checked the image, switched its boot slot and is restarting.

use std::ops::Range;

use serde::Deserialize;

/// The frames and replies this app speaks (`OTA_PROTO_VERSION`).
pub const PROTO_VERSION: u32 = 1;

const OP_BEGIN: u8 = 0x01;
const OP_DATA: u8 = 0x02;
/// END: every byte is in; check the image and restart into it.
pub const END_FRAME: [u8; 1] = [0x03];
/// ABORT: drop the update.
pub const ABORT_FRAME: [u8; 1] = [0x04];
/// DATA frame header: op and the u32 offset.
pub const DATA_HEADER: usize = 5;
/// The longest env BEGIN may carry (`OTA_ENV_MAX`).
pub const ENV_MAX: usize = 40;

/// What the board's INFO characteristic says about itself.
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct DeviceInfo {
    /// PlatformIO env: which release image fits it.
    pub env: String,
    /// Firmware version (`v1.10.0`, `dev` for a local build).
    pub ver: String,
    /// Board revision, where it has more than one.
    #[serde(default)]
    pub rev: String,
    /// Bytes an update may take; 0 when the board has no spare app slot
    /// (it updates over USB only).
    #[serde(rename = "ota", default)]
    pub ota_slot: u32,
    /// The update protocol it speaks.
    #[serde(rename = "otav", default)]
    pub ota_proto: u32,
}

impl DeviceInfo {
    /// Parse the characteristic's JSON.
    pub fn parse(value: &[u8]) -> Option<Self> {
        serde_json::from_slice(value).ok()
    }

    /// It takes updates over Bluetooth, in a protocol this app speaks.
    pub fn can_update(&self) -> bool {
        self.ota_slot > 0 && self.ota_proto == PROTO_VERSION
    }
}

/// BEGIN: the image's size and SHA-256, and the env it was built for (the
/// board refuses another board's image).
pub fn begin_frame(size: u32, sha256: &[u8; 32], env: &str) -> Vec<u8> {
    let mut f = Vec::with_capacity(1 + 4 + 32 + env.len());
    f.push(OP_BEGIN);
    f.extend(size.to_le_bytes());
    f.extend(sha256);
    f.extend(env.as_bytes());
    f
}

/// DATA: `payload` at `offset` into the image.
pub fn data_frame(offset: u32, payload: &[u8]) -> Vec<u8> {
    let mut f = Vec::with_capacity(DATA_HEADER + payload.len());
    f.push(OP_DATA);
    f.extend(offset.to_le_bytes());
    f.extend(payload);
    f
}

/// A board's reply, notified on TX.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Reply {
    /// BEGIN is valid; the board asks on its screen and waits up to `secs`
    /// seconds for a tap or button press to allow it.
    Confirm {
        /// How long it waits.
        secs: u8,
    },
    /// BEGIN accepted: send data. `window` is how many unacknowledged bytes
    /// it can hold.
    Ready {
        /// The link's ATT MTU.
        mtu: u16,
        /// Receive buffer size.
        window: u32,
    },
    /// Bytes written to flash so far.
    At(u32),
    /// A chunk went missing: send again from here.
    Resend(u32),
    /// Checked, boot slot switched, restarting.
    Done,
    /// Stopped (an `OtaErr` code, see [`error_message`]).
    Error(u8),
}

/// Whether a TX notification is an update reply rather than a JSON ack:
/// replies start at 0x80, JSON with `{`.
pub fn is_reply(value: &[u8]) -> bool {
    value.first().is_some_and(|b| *b >= 0x80)
}

impl Reply {
    /// Read one notification.
    pub fn parse(v: &[u8]) -> Option<Reply> {
        let u32_at = |i: usize| v.get(i..i + 4).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]));
        match *v.first()? {
            0x81 => Some(Reply::Ready { mtu: u16::from_le_bytes([*v.get(1)?, *v.get(2)?]), window: u32_at(3)? }),
            0x82 => Some(Reply::At(u32_at(1)?)),
            0x83 => Some(Reply::Resend(u32_at(1)?)),
            0x84 => Some(Reply::Done),
            0x85 => Some(Reply::Error(*v.get(1)?)),
            0x86 => Some(Reply::Confirm { secs: *v.get(1)? }),
            _ => None,
        }
    }
}

/// What an ERROR code means, for the user.
pub fn error_message(code: u8) -> &'static str {
    match code {
        1 => "This board can only be updated over USB.",
        2 => "The firmware is too large for this board's update slot.",
        3 => "This firmware is for a different board.",
        4 => "The board didn't understand the update request.",
        5 => "The board couldn't write its flash.",
        6 => "The firmware arrived damaged (checksum mismatch).",
        7 => "The board rejected the firmware image.",
        8 => "The board had no update in progress.",
        9 => "The board was missing part of the firmware.",
        10 => "The board ran out of memory for the update.",
        11 => "The board stopped waiting for data.",
        12 => "The update wasn't allowed on the board in time. Tap its screen (or press a button twice) when it asks.",
        13 => "The board is busy with another update request, or refused one moments ago. Try again in half a minute.",
        _ => "The board stopped the update.",
    }
}

/// DATA payload bytes per frame for an ATT `mtu`: what fits one write
/// without response (MTU minus the 3-byte ATT header) after the frame's
/// own header, capped at the 512-byte attribute limit.
pub fn chunk_size(mtu: u16) -> usize {
    (usize::from(mtu).saturating_sub(3)).clamp(20, 512) - DATA_HEADER
}

/// The sending side's view of the image: what the board has written to
/// flash (`acked`), what goes out next, and the window between them.
#[derive(Debug, Clone)]
pub struct Sender {
    size: u32,
    chunk: u32,
    window: u32,
    acked: u32,
    next: u32,
}

impl Sender {
    /// `size` bytes in frames of `chunk`, with at most `window` bytes
    /// unacknowledged (never less than one chunk).
    pub fn new(size: u32, chunk: usize, window: u32) -> Self {
        let chunk = (chunk.max(1) as u32).min(size.max(1));
        Sender { size, chunk, window: window.max(chunk), acked: 0, next: 0 }
    }

    /// The next range to send, if the window allows one.
    pub fn next_chunk(&mut self) -> Option<Range<usize>> {
        if self.next >= self.size {
            return None;
        }
        let n = self.chunk.min(self.size - self.next);
        if self.next - self.acked + n > self.window {
            return None;
        }
        let r = self.next as usize..(self.next + n) as usize;
        self.next += n;
        Some(r)
    }

    /// Take an AT or RESEND.
    pub fn on_reply(&mut self, r: Reply) {
        match r {
            Reply::At(at) => {
                self.acked = self.acked.max(at.min(self.size));
                self.next = self.next.max(self.acked);
            }
            // Everything before `at` arrived; whatever was sent after it
            // was dropped, so go back. (A RESEND behind an AT that already
            // passed it is stale.)
            Reply::Resend(at) if at >= self.acked && at < self.next => self.next = at,
            _ => {}
        }
    }

    /// Nothing came back for a while: the frames since the last AT may be
    /// lost (the last one has no later frame to reveal the gap), so send
    /// them again.
    pub fn rewind(&mut self) {
        self.next = self.acked;
    }

    /// Bytes the board has written.
    pub fn acked(&self) -> u32 {
        self.acked
    }

    /// Every byte is written: time for END.
    pub fn complete(&self) -> bool {
        self.acked >= self.size
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn frames_match_the_firmware_layout() {
        let f = begin_frame(0x0102_0304, &[7; 32], "sim");
        assert_eq!(f[0], 0x01);
        assert_eq!(&f[1..5], &[4, 3, 2, 1]);
        assert_eq!(&f[5..37], &[7; 32]);
        assert_eq!(&f[37..], b"sim");
        assert_eq!(data_frame(4096, &[0xAA]), vec![0x02, 0x00, 0x10, 0x00, 0x00, 0xAA]);
    }

    #[test]
    fn replies_parse_and_are_told_apart_from_json() {
        assert_eq!(
            Reply::parse(&[0x81, 0x00, 0x01, 0x00, 0x40, 0x00, 0x00]),
            Some(Reply::Ready { mtu: 256, window: 16384 })
        );
        assert_eq!(Reply::parse(&[0x82, 4, 3, 2, 1]), Some(Reply::At(0x0102_0304)));
        assert_eq!(Reply::parse(&[0x83, 0, 1, 0, 0]), Some(Reply::Resend(256)));
        assert_eq!(Reply::parse(&[0x84]), Some(Reply::Done));
        assert_eq!(Reply::parse(&[0x85, 6]), Some(Reply::Error(6)));
        assert_eq!(Reply::parse(&[0x86, 30]), Some(Reply::Confirm { secs: 30 }));
        assert!(error_message(12).contains("allowed"));
        assert!(error_message(13).contains("busy"));
        assert_eq!(Reply::parse(&[0x82, 1]), None, "short");
        assert!(is_reply(&[0x84]));
        assert!(!is_reply(br#"{"ack":true}"#));
        assert!(!is_reply(&[]));
    }

    #[test]
    fn info_reads_the_firmware_json() {
        let i = DeviceInfo::parse(br#"{"env":"waveshare_amoled_216","ver":"v1.10.0","rev":"","ota":3342336,"otav":1}"#)
            .unwrap();
        assert_eq!(i.env, "waveshare_amoled_216");
        assert!(i.can_update());
        let usb_only = DeviceInfo::parse(br#"{"env":"esp32_2432s028r","ver":"v1.10.0","ota":0,"otav":1}"#).unwrap();
        assert!(!usb_only.can_update());
        let future = DeviceInfo::parse(br#"{"env":"x","ver":"v9","ota":1,"otav":2}"#).unwrap();
        assert!(!future.can_update(), "a protocol this app doesn't speak");
        assert_eq!(DeviceInfo::parse(b"nope"), None);
    }

    #[test]
    fn info_keys_match_the_firmware() {
        let src = std::fs::read_to_string(
            std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../firmware/src/ble.cpp"),
        );
        if let Ok(src) = src {
            for k in ["env", "ver", "rev", "ota", "otav"] {
                assert!(src.contains(&format!(r#"\"{k}\":"#)), "INFO key {k} not in ble.cpp");
            }
        }
        let proto = std::fs::read_to_string(
            std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../firmware/src/ota_proto.h"),
        );
        if let Ok(proto) = proto {
            assert!(proto.contains(&format!("#define OTA_PROTO_VERSION {PROTO_VERSION}")));
            assert!(proto.contains(&format!("#define OTA_ENV_MAX     {ENV_MAX}")));
        }
    }

    #[test]
    fn chunks_fit_the_mtu() {
        assert_eq!(chunk_size(256), 248, "macOS: 253-byte writes");
        assert_eq!(chunk_size(23), 15, "the default MTU");
        assert_eq!(chunk_size(517), 507);
        assert_eq!(chunk_size(0), 15);
    }

    #[test]
    fn the_window_bounds_what_is_in_flight() {
        let mut s = Sender::new(1000, 100, 300);
        assert_eq!(s.next_chunk(), Some(0..100));
        assert_eq!(s.next_chunk(), Some(100..200));
        assert_eq!(s.next_chunk(), Some(200..300));
        assert_eq!(s.next_chunk(), None, "window full");
        s.on_reply(Reply::At(200));
        assert_eq!(s.next_chunk(), Some(300..400));
        assert_eq!(s.next_chunk(), Some(400..500));
        assert_eq!(s.next_chunk(), None);
    }

    #[test]
    fn resend_and_silence_rewind() {
        let mut s = Sender::new(1000, 100, 1000);
        for _ in 0..5 {
            s.next_chunk();
        }
        s.on_reply(Reply::Resend(200));
        assert_eq!(s.next_chunk(), Some(200..300));
        s.on_reply(Reply::At(300));
        s.on_reply(Reply::Resend(200));
        assert_eq!(s.next_chunk(), Some(300..400), "a RESEND behind the last AT is stale");
        s.rewind();
        assert_eq!(s.next_chunk(), Some(300..400), "silence: back to the last AT");
    }

    #[test]
    fn the_tail_chunk_is_short_and_completes() {
        let mut s = Sender::new(250, 100, 1000);
        assert_eq!(s.next_chunk(), Some(0..100));
        assert_eq!(s.next_chunk(), Some(100..200));
        assert_eq!(s.next_chunk(), Some(200..250));
        assert_eq!(s.next_chunk(), None);
        assert!(!s.complete());
        s.on_reply(Reply::At(250));
        assert!(s.complete());
    }

    /// The firmware's receive path (ota_proto.cpp's ota_rx_chunk and ring),
    /// ported, against the sender over a link that loses chunks.
    #[test]
    fn a_lossy_link_still_delivers_every_byte() {
        let image: Vec<u8> = (0..60_000u32).map(|i| (i * 7 + (i >> 8)) as u8).collect();
        let (cap, size) = (8192u32, image.len() as u32);
        let mut s = Sender::new(size, chunk_size(256), cap);
        let (mut expected, mut resend_for) = (0u32, u32::MAX);
        let mut ring: std::collections::VecDeque<u8> = Default::default();
        let (mut flash, mut last_ack) = (Vec::new(), 0u32);
        let mut seed = 7u32;
        for _ in 0..500_000 {
            if s.complete() {
                break;
            }
            let mut replies = Vec::new();
            if let Some(r) = s.next_chunk() {
                seed = seed.wrapping_mul(1_103_515_245).wrapping_add(12345);
                if !(seed >> 16).is_multiple_of(7) {
                    let (off, n) = (r.start as u32, r.len() as u32);
                    let room = cap - ring.len() as u32;
                    if off == expected && n <= room {
                        ring.extend(&image[r]);
                        expected += n;
                        resend_for = u32::MAX;
                    } else if off >= expected && resend_for != expected {
                        // A gap, or the buffer is full: ask once per offset.
                        resend_for = expected;
                        replies.push(Reply::Resend(expected));
                    }
                }
            }
            // Flash: up to 400 bytes a step.
            for _ in 0..400 {
                match ring.pop_front() {
                    Some(b) => flash.push(b),
                    None => break,
                }
            }
            let written = flash.len() as u32;
            if written > last_ack && (written - last_ack >= 2048 || written == size) {
                last_ack = written;
                replies.push(Reply::At(written));
            }
            let quiet = replies.is_empty();
            for r in replies {
                s.on_reply(r);
            }
            if quiet && ring.is_empty() && s.next_chunk_would_wait() {
                s.rewind();
            }
        }
        assert!(s.complete(), "acked {} of {size}", s.acked());
        assert_eq!(flash, image);
    }

    impl Sender {
        /// Test helper: the sender has nothing it may send right now.
        fn next_chunk_would_wait(&self) -> bool {
            let mut c = self.clone();
            c.next_chunk().is_none()
        }
    }
}
