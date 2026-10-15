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
pub const BOARDS: [BoardInfo; 25] = [
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
    BoardInfo {
        env: "lilygo_t4s3",
        name: "T4-S3",
        product: "LILYGO T4-S3",
        width: 450,
        height: 600,
        buy_url: "https://lilygo.cc/products/t4-s3",
        summary: "450×600 · AMOLED",
        chip: "ESP32-S3",
        pwr_button: false,
    },
    BoardInfo {
        env: "waveshare_lcd_183",
        name: "LCD 1.83",
        product: "Waveshare ESP32-S3-Touch-LCD-1.83",
        width: 240,
        height: 284,
        buy_url: "https://www.waveshare.com/esp32-s3-touch-lcd-1.83.htm?&aff_id=149786",
        summary: "240×284 · SPI TFT",
        chip: "ESP32-S3",
        pwr_button: true,
    },
    BoardInfo {
        env: "esp32_s3_4848s040",
        name: "4848S040",
        product: "Guition ESP32-S3-4848S040 (4-inch 480×480)",
        width: 480,
        height: 480,
        buy_url: "https://www.aliexpress.com/item/3256806115962222.html",
        summary: "480×480 · RGB panel",
        chip: "ESP32-S3",
        pwr_button: false,
    },
    BoardInfo {
        env: "spotpear_s3_154",
        name: "SpotPear 1.54 MUMA",
        product: "SpotPear ESP32-S3-LCD-1.54 MUMA (touch)",
        width: 240,
        height: 240,
        buy_url: "https://spotpear.com/shop/ESP32-S3-AI-1.54-inch-LCD-Display-TouchScreen-N16R8-muma-DeepSeek.html",
        summary: "240×240 · SPI LCD",
        chip: "ESP32-S3",
        pwr_button: false,
    },
    BoardInfo {
        env: "sensecap_indicator_d1l",
        name: "SenseCAP Indicator D1L",
        product: "Seeed Studio SenseCAP Indicator D1L",
        width: 480,
        height: 480,
        buy_url: "https://www.seeedstudio.com/SenseCAP-Indicator-D1L-p-5646.html",
        summary: "480×480 · RGB TFT",
        chip: "ESP32-S3",
        pwr_button: true,
    },
    BoardInfo {
        env: "freenove_s3_display_nontouch",
        name: "S3 Display (no touch)",
        product: "Freenove ESP32-S3 Display 2.8 inch (non-touch, FNK0104A)",
        width: 240,
        height: 320,
        buy_url: "https://store.freenove.com/products/fnk0104",
        summary: "240×320 · SPI TFT",
        chip: "ESP32-S3",
        pwr_button: true,
    },
    BoardInfo {
        env: "xingzhi_cube_183",
        name: "Xingzhi Cube 1.83",
        product: "Xingzhi Cube 1.83 inch TFT WiFi (2-mic)",
        width: 284,
        height: 240,
        buy_url: "https://www.aliexpress.com/item/1005010442374066.html",
        summary: "284×240 · SPI TFT",
        chip: "ESP32-S3",
        pwr_button: true,
    },
    BoardInfo {
        env: "waveshare_epaper_154",
        name: "ePaper 1.54",
        product: "Waveshare ESP32-S3-ePaper-1.54",
        width: 200,
        height: 200,
        buy_url: "https://www.waveshare.com/esp32-s3-epaper-1.54.htm?&aff_id=149786",
        summary: "200×200 · e-paper",
        chip: "ESP32-S3",
        pwr_button: true,
    },
    BoardInfo {
        env: "lilygo_t_display_s3",
        name: "T-Display S3",
        product: "LILYGO T-Display S3",
        width: 170,
        height: 320,
        buy_url: "https://lilygo.cc/products/t-display-s3",
        summary: "170×320 · 2 buttons",
        chip: "ESP32-S3",
        pwr_button: true,
    },
    BoardInfo {
        env: "waveshare_lcd_2",
        name: "LCD 2",
        product: "Waveshare ESP32-S3-Touch-LCD-2",
        width: 240,
        height: 320,
        buy_url: "https://www.waveshare.com/esp32-s3-touch-lcd-2.htm?&aff_id=149786",
        summary: "240×320 · touch only",
        chip: "ESP32-S3",
        pwr_button: false,
    },
    BoardInfo {
        env: "m5stack_fire",
        name: "M5Stack FIRE",
        product: "M5Stack FIRE IoT Development Kit (PSRAM) V2.7",
        width: 320,
        height: 240,
        buy_url: "https://shop.m5stack.com/products/m5stack-fire-iot-development-kit-psram-v2-7",
        summary: "320×240 · 3 buttons · no touch",
        chip: "ESP32",
        pwr_button: true,
    },
    BoardInfo {
        env: "m5sticks3",
        name: "M5StickS3",
        product: "M5Stack M5StickS3",
        width: 135,
        height: 240,
        buy_url: "https://shop.m5stack.com/products/m5sticks3-esp32s3-mini-iot-dev-kit",
        summary: "135×240 · no touch",
        chip: "ESP32-S3",
        pwr_button: true,
    },
    BoardInfo {
        env: "esp32_2432s028r",
        name: "CYD 2.8",
        product: "ESP32-2432S028R Cheap Yellow Display (portrait)",
        width: 240,
        height: 320,
        buy_url: "https://www.amazon.com/dp/B0BVFXR313",
        summary: "240×320 · resistive touch · 1 button",
        chip: "ESP32",
        pwr_button: true,
    },
    BoardInfo {
        env: "esp32_2432s028r_landscape",
        name: "CYD 2.8 landscape",
        product: "ESP32-2432S028R Cheap Yellow Display (landscape)",
        width: 320,
        height: 240,
        buy_url: "https://www.amazon.com/dp/B0BVFXR313",
        summary: "320×240 · resistive touch · 1 button",
        chip: "ESP32",
        pwr_button: true,
    },
    BoardInfo {
        env: "esp32_2432s024c",
        name: "CYD 2.4",
        product: "ESP32-2432S024C Cheap Yellow Display (portrait)",
        width: 240,
        height: 320,
        buy_url: "https://www.amazon.com/dp/B0CLGD2DG6",
        summary: "240×320 · capacitive touch · 1 button",
        chip: "ESP32",
        pwr_button: true,
    },
    BoardInfo {
        env: "esp32_2432s024c_landscape",
        name: "CYD 2.4 landscape",
        product: "ESP32-2432S024C Cheap Yellow Display (landscape)",
        width: 320,
        height: 240,
        buy_url: "https://www.amazon.com/dp/B0CLGD2DG6",
        summary: "320×240 · capacitive touch · 1 button",
        chip: "ESP32",
        pwr_button: true,
    },
    BoardInfo {
        env: "waveshare_amoled_132",
        name: "AMOLED 1.32",
        product: "Waveshare ESP32-S3-Touch-AMOLED-1.32",
        width: 466,
        height: 466,
        buy_url: "https://www.waveshare.com/esp32-s3-touch-amoled-1.32.htm?&aff_id=149786",
        summary: "466×466 · round",
        chip: "ESP32-S3",
        pwr_button: true,
    },
    BoardInfo {
        env: "waveshare_amoled_143",
        name: "AMOLED 1.43",
        product: "Waveshare ESP32-S3-Touch-AMOLED-1.43",
        width: 466,
        height: 466,
        buy_url: "https://www.waveshare.com/esp32-s3-touch-amoled-1.43.htm?&aff_id=149786",
        summary: "466×466 · round",
        chip: "ESP32-S3",
        pwr_button: false,
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
            assert!(b.buy_url.starts_with("https://"), "{}", b.env);
        }
    }
}
