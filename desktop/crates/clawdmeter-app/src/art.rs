//! The official idle Clawd (extracted from the firmware's art at build time)
//! and the tray icon rasterizer.
//!
//! Everywhere in the app Clawd is drawn at whole cells with no smoothing; the
//! tray icon is the one place he's point-sampled below one pixel per cell.

include!(concat!(env!("OUT_DIR"), "/clawd_idle.rs"));

/// Palette index of a cell of the idle body (0 = transparent).
pub fn idle_cell(col: usize, row: usize) -> u8 {
    IDLE_CELLS[row * IDLE_W + col]
}

/// True when a palette entry is the eye ink (the converter's eyes are pure
/// black inside the body; index 0 is the background, not an eye).
pub fn is_eye(index: u8) -> bool {
    index != 0 && IDLE_PALETTE[index as usize] == 0
}

/// Colour of a palette index as 0xRRGGBB. Eyes are drawn in the theme's eye
/// ink (#141413) so they read on a black background as they do on the panel.
pub fn cell_rgb(index: u8) -> u32 {
    if is_eye(index) {
        EYE_INK
    } else {
        IDLE_PALETTE[index as usize]
    }
}

/// `--eye` in the design tokens.
pub const EYE_INK: u32 = 0x141413;

/// What the tray icon shows.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum TrayState {
    /// Fresh data reaching the device.
    Connected,
    /// No fresh data for `DATA_FRESH_MS` (90 s) or the polls are failing.
    Stale,
    /// Looking for the device (or Bluetooth is off).
    Searching,
    /// The `claude` CLI is logged out.
    AuthError,
    /// A session is waiting for the user: full-colour Clawd, no dot.
    Attention,
}

impl TrayState {
    /// Dot colour, 0xRRGGBB (None for attention).
    pub fn dot(self) -> Option<u32> {
        match self {
            TrayState::Connected => Some(0x788c5d),
            TrayState::Stale => Some(0xd4a24c),
            TrayState::Searching => Some(0x7a7870),
            TrayState::AuthError => Some(0xd4553f),
            TrayState::Attention => None,
        }
    }

    /// Short label for the status line.
    pub fn label(self) -> &'static str {
        match self {
            TrayState::Connected => "Connected",
            TrayState::Stale => "Usage out of date",
            TrayState::Searching => "Looking for device",
            TrayState::AuthError => "Signed out",
            TrayState::Attention => "Claude needs you",
        }
    }
}

/// Monochrome body colour on the tray (a light neutral that reads on both
/// dark and light panels).
pub const TRAY_MONO: u32 = 0xd8d6cf;
/// Full-colour body for attention (the device's Clawd orange).
pub const TRAY_ATTENTION: u32 = 0xde7552;

/// Rasterize the tray icon as straight RGBA, `size` x `size` (16 or 32; any
/// multiple of 16 works). The 24x16 body is point-sampled to 16x11 at 1.5
/// cells per pixel, sitting 2 px from the top; the state dot is a 4 px swatch
/// in a 1 px black ring (6 px) in the lower right.
pub fn tray_icon_rgba(state: TrayState, size: u32) -> Vec<u8> {
    let k = (size / 16).max(1) as usize;
    let s = 16 * k;
    let mut px = vec![0u8; s * s * 4];
    let mut put = |x: usize, y: usize, rgb: u32| {
        for dy in 0..k {
            for dx in 0..k {
                let i = ((y * k + dy) * s + (x * k + dx)) * 4;
                px[i] = (rgb >> 16) as u8;
                px[i + 1] = (rgb >> 8) as u8;
                px[i + 2] = rgb as u8;
                px[i + 3] = 0xff;
            }
        }
    };
    let body = if state == TrayState::Attention { TRAY_ATTENTION } else { TRAY_MONO };
    for y in 0..11 {
        for x in 0..16 {
            let (sc, sr) = ((x * 3) / 2, (y * 3) / 2);
            let v = idle_cell(sc, sr);
            if v == 0 {
                continue;
            }
            put(x, y + 2, if is_eye(v) { 0x000000 } else { body });
        }
    }
    if let Some(dot) = state.dot() {
        for y in 9..15 {
            for x in 10..16 {
                put(x, y, 0x000000);
            }
        }
        for y in 10..14 {
            for x in 11..15 {
                put(x, y, dot);
            }
        }
    }
    px
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn idle_body_is_the_official_frame() {
        assert_eq!((IDLE_W, IDLE_H), (24, 16));
        // Two eyes, symmetric enough to read as a face.
        let eyes: Vec<(usize, usize)> = (0..IDLE_H)
            .flat_map(|r| (0..IDLE_W).map(move |c| (c, r)))
            .filter(|&(c, r)| is_eye(idle_cell(c, r)))
            .collect();
        assert!(!eyes.is_empty(), "idle frame has eye cells");
        let filled = IDLE_CELLS.iter().filter(|&&v| v != 0).count();
        assert!(filled > 150, "body is mostly filled ({filled})");
    }

    fn pixel(img: &[u8], size: usize, x: usize, y: usize) -> [u8; 4] {
        let i = (y * size + x) * 4;
        [img[i], img[i + 1], img[i + 2], img[i + 3]]
    }

    #[test]
    fn eyes_survive_at_16px() {
        let img = tray_icon_rgba(TrayState::Connected, 16);
        assert_eq!(img.len(), 16 * 16 * 4);
        // Black opaque pixels inside the body area (excluding the dot ring).
        let dark = (0..16)
            .flat_map(|y| (0..16).map(move |x| (x, y)))
            .filter(|&(x, y)| !(x >= 10 && y >= 9))
            .filter(|&(x, y)| pixel(&img, 16, x, y) == [0, 0, 0, 255])
            .count();
        assert!(dark >= 2, "two dark eye pixels, found {dark}");
    }

    #[test]
    fn dot_colours_and_attention() {
        let img = tray_icon_rgba(TrayState::Stale, 32);
        assert_eq!(pixel(&img, 32, 24, 22), [0xd4, 0xa2, 0x4c, 255]);
        let att = tray_icon_rgba(TrayState::Attention, 16);
        let orange = att.chunks(4).filter(|p| p == &[0xde, 0x75, 0x52, 255]).count();
        assert!(orange > 60, "attention is full colour");
        assert!(TrayState::Attention.dot().is_none());
        assert!(!att.chunks(4).any(|p| p == [0x78, 0x8c, 0x5d, 255]), "no state dot on attention");
    }
}
