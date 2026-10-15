//! What the app knows about each supported board: names, a short summary,
//! where to buy one, and its illustration (`assets/boards/<env>.svg`, drawn
//! for the app; bundled into the binary by [`crate::ui::Assets`]).

/// One supported board.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct BoardInfo {
    /// PlatformIO env.
    pub env: &'static str,
    /// Short name.
    pub name: &'static str,
    /// Full product name.
    pub product: &'static str,
    /// Panel width, px.
    pub width: u32,
    /// Panel height, px.
    pub height: u32,
    /// Where to buy one.
    pub buy_url: &'static str,
    /// One-line summary.
    pub summary: &'static str,
    /// SoC.
    pub chip: &'static str,
    /// Has a PWR button the firmware reads (hold 3 s, release: clear the
    /// pairing). The LCD-4's KEY/PWR is a hardware reset; it clears the
    /// pairing with a 5 s hold on the screen while the pairing hint shows.
    pub pwr_button: bool,
}

/// Every hardware env in `firmware/platformio.ini` (not `sim` / `native`).
pub const BOARDS: [BoardInfo; 7] = [
    BoardInfo {
        env: "waveshare_amoled_216",
        name: "AMOLED 2.16",
        product: "Waveshare ESP32-S3-Touch-AMOLED-2.16",
        width: 480,
        height: 480,
        buy_url: "https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm?&aff_id=149786",
        summary: "480×480 · 3 buttons",
        chip: "ESP32-S3",
        pwr_button: true,
    },
    BoardInfo {
        env: "waveshare_amoled_18",
        name: "AMOLED 1.8",
        product: "Waveshare ESP32-S3-Touch-AMOLED-1.8",
        width: 368,
        height: 448,
        buy_url: "https://www.waveshare.com/esp32-s3-touch-amoled-1.8.htm?&aff_id=149786",
        summary: "368×448 · 2 buttons",
        chip: "ESP32-S3",
        pwr_button: true,
    },
    BoardInfo {
        env: "waveshare_amoled_206",
        name: "AMOLED 2.06",
        product: "Waveshare ESP32-S3-Touch-AMOLED-2.06",
        width: 410,
        height: 502,
        buy_url: "https://www.waveshare.com/esp32-s3-touch-amoled-2.06.htm?&aff_id=149786",
        summary: "410×502 · watch",
        chip: "ESP32-S3",
        pwr_button: true,
    },
    BoardInfo {
        env: "waveshare_lcd_154",
        name: "LCD 1.54",
        product: "Waveshare ESP32-S3-Touch-LCD-1.54",
        width: 240,
        height: 240,
        buy_url: "https://www.waveshare.com/esp32-s3-lcd-1.54.htm?sku=33869&aff_id=149786",
        summary: "240×240 · SPI TFT",
        chip: "ESP32-S3",
        pwr_button: true,
    },
    BoardInfo {
        env: "waveshare_lcd_4",
        name: "LCD 4\"",
        product: "Waveshare ESP32-S3-Touch-LCD-4",
        width: 480,
        height: 480,
        buy_url: "https://www.waveshare.com/esp32-s3-touch-lcd-4.htm",
        summary: "480×480 · RGB TFT",
        chip: "ESP32-S3",
        pwr_button: false,
    },
    BoardInfo {
        env: "waveshare_amoled_216_c6",
        name: "AMOLED 2.16 (C6)",
        product: "Waveshare ESP32-C6-Touch-AMOLED-2.16",
        width: 480,
        height: 480,
        buy_url: "https://www.waveshare.com/esp32-c6-touch-amoled-2.16.htm?&aff_id=149786",
        summary: "480×480 · ESP32-C6",
        chip: "ESP32-C6",
        pwr_button: true,
    },
    BoardInfo {
        env: "waveshare_amoled_18_c6",
        name: "AMOLED 1.8 (C6)",
        product: "Waveshare ESP32-C6-Touch-AMOLED-1.8",
        width: 368,
        height: 448,
        buy_url: "https://www.waveshare.com/esp32-c6-touch-amoled-1.8.htm?&aff_id=149786",
        summary: "368×448 · ESP32-C6",
        chip: "ESP32-C6",
        pwr_button: true,
    },
];

/// Look up a board by env.
pub fn board(env: &str) -> Option<&'static BoardInfo> {
    BOARDS.iter().find(|b| b.env == env)
}

impl BoardInfo {
    /// The illustration's asset path (see [`crate::ui::Assets`]).
    pub fn illustration(&self) -> String {
        format!("boards/{}.svg", self.env)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_firmware_env_is_listed() {
        let ini = include_str!("../../../../firmware/platformio.ini");
        for line in ini.lines() {
            if let Some(env) = line.strip_prefix("[env:").and_then(|l| l.strip_suffix(']')) {
                if env == "sim" || env == "native" {
                    continue;
                }
                assert!(board(env).is_some(), "{env} missing from BOARDS");
            }
        }
    }

    #[test]
    fn every_board_has_art_and_a_shop() {
        for b in BOARDS {
            assert!(crate::ui::Assets::get(&b.illustration()).is_some(), "{} has no illustration", b.env);
            assert!(b.buy_url.starts_with("https://www.waveshare.com/"), "{}", b.env);
        }
    }
}
