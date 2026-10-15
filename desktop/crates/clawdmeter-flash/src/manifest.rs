//! The release `manifest.json` CI publishes next to the merged images:
//!
//! ```json
//! {"version": "1.9.0",
//!  "boards": [{"env": "waveshare_amoled_216", "chip": "esp32s3", "flash_size": "16MB",
//!              "display_name": "Waveshare AMOLED 2.16\"", "file": "waveshare_amoled_216-v1.9.0.bin",
//!              "sha256": "…"}]}
//! ```
//!
//! `chip` and `flash_size` are read leniently (`esp32s3` / `ESP32-S3`,
//! `"16MB"` / `16` / `16777216`) since they are compared, not displayed.

use std::cmp::Ordering;

use serde::{Deserialize, Deserializer, Serialize};

/// Top-level manifest.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Manifest {
    /// Firmware version, e.g. `1.9.0` (a leading `v` is tolerated).
    pub version: String,
    /// One merged image per hardware env.
    pub boards: Vec<BoardImage>,
}

/// One board's merged image (bootloader + partitions + boot_app0 + app,
/// written at 0x0).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct BoardImage {
    /// PlatformIO env name.
    pub env: String,
    /// SoC, e.g. `esp32s3`.
    pub chip: String,
    /// Flash size as published (kept verbatim for re-serialization).
    #[serde(deserialize_with = "flash_size_text")]
    pub flash_size: String,
    /// Human name for the picker.
    pub display_name: String,
    /// Asset file name.
    pub file: String,
    /// Hex SHA-256 of the file.
    pub sha256: String,
}

fn flash_size_text<'de, D: Deserializer<'de>>(d: D) -> Result<String, D::Error> {
    let v = serde_json::Value::deserialize(d)?;
    Ok(match v {
        serde_json::Value::String(s) => s,
        other => other.to_string(),
    })
}

/// Normalize a chip name: `ESP32-S3` / `esp32s3` / `Esp32s3` → `esp32s3`.
pub fn normalize_chip(s: &str) -> String {
    s.chars().filter(|c| c.is_ascii_alphanumeric()).collect::<String>().to_ascii_lowercase()
}

/// Flash size in MB from `16MB`, `16 MB`, `16`, `16M`, or a byte count.
pub fn flash_mb(s: &str) -> Option<u32> {
    let t = s.trim().to_ascii_uppercase();
    let digits: String = t.chars().take_while(|c| c.is_ascii_digit()).collect();
    let n: u64 = digits.parse().ok()?;
    let rest = t[digits.len()..].trim();
    match rest {
        "" if n >= 1024 * 1024 => Some((n / (1024 * 1024)) as u32),
        "" | "M" | "MB" | "MIB" => Some(n as u32),
        "KB" | "K" => Some((n / 1024) as u32),
        _ => None,
    }
}

impl BoardImage {
    /// Normalized chip.
    pub fn chip_id(&self) -> String {
        normalize_chip(&self.chip)
    }

    /// Flash size in MB, if parseable.
    pub fn flash_mb(&self) -> Option<u32> {
        flash_mb(&self.flash_size)
    }
}

impl Manifest {
    /// Parse manifest JSON.
    pub fn parse(json: &str) -> Result<Self, serde_json::Error> {
        serde_json::from_str(json)
    }

    /// The image for an env.
    pub fn board(&self, env: &str) -> Option<&BoardImage> {
        self.boards.iter().find(|b| b.env == env)
    }

    /// Boards that fit a probed chip and flash size (probe 2 of the
    /// flasher). An image fits when it needs no more flash than the board
    /// has: a 16 MB board can run an 8 MB image (some boards ship with more
    /// flash than their env assumes). Exact size matches come first, then
    /// smaller images, largest first; manifest order breaks ties. A board
    /// whose flash size is unknown on either side stays in the list, last.
    pub fn candidates(&self, chip: &str, flash_mb: Option<u32>) -> Vec<&BoardImage> {
        let chip = normalize_chip(chip);
        let mut out: Vec<(u8, u32, usize, &BoardImage)> = self
            .boards
            .iter()
            .enumerate()
            .filter(|(_, b)| b.chip_id() == chip)
            .filter_map(|(i, b)| match (flash_mb, b.flash_mb()) {
                (Some(have), Some(need)) if need == have => Some((0, 0, i, b)),
                (Some(have), Some(need)) if need < have => Some((1, have - need, i, b)),
                (Some(_), Some(_)) => None,
                _ => Some((2, 0, i, b)),
            })
            .collect();
        out.sort_by_key(|(rank, gap, i, _)| (*rank, *gap, *i));
        out.into_iter().map(|(_, _, _, b)| b).collect()
    }
}

/// Compare dotted versions numerically (`v1.10.0` > `1.9.2`); a pre-release
/// suffix (`-rc1`) sorts before the release.
pub fn compare_versions(a: &str, b: &str) -> Ordering {
    fn split(v: &str) -> (Vec<u64>, Option<&str>) {
        let v = v.trim().trim_start_matches(['v', 'V']);
        let (core, pre) = match v.split_once(['-', '+']) {
            Some((c, p)) => (c, Some(p)),
            None => (v, None),
        };
        (core.split('.').map(|p| p.parse().unwrap_or(0)).collect(), pre)
    }
    let (ca, pa) = split(a);
    let (cb, pb) = split(b);
    let n = ca.len().max(cb.len());
    for i in 0..n {
        let x = ca.get(i).copied().unwrap_or(0);
        let y = cb.get(i).copied().unwrap_or(0);
        match x.cmp(&y) {
            Ordering::Equal => {}
            o => return o,
        }
    }
    match (pa, pb) {
        (None, None) => Ordering::Equal,
        (None, Some(_)) => Ordering::Greater,
        (Some(_), None) => Ordering::Less,
        (Some(x), Some(y)) => compare_pre(x, y),
    }
}

/// Semver pre-release order: dot-separated identifiers, numeric ones by
/// value (`rc.10` after `rc.9`), numeric before alphanumeric, fewer before
/// more when one is a prefix of the other.
fn compare_pre(a: &str, b: &str) -> Ordering {
    let mut xs = a.split('.');
    let mut ys = b.split('.');
    loop {
        let o = match (xs.next(), ys.next()) {
            (None, None) => return Ordering::Equal,
            (None, Some(_)) => return Ordering::Less,
            (Some(_), None) => return Ordering::Greater,
            (Some(x), Some(y)) => match (x.parse::<u64>(), y.parse::<u64>()) {
                (Ok(m), Ok(n)) => m.cmp(&n),
                (Ok(_), Err(_)) => Ordering::Less,
                (Err(_), Ok(_)) => Ordering::Greater,
                (Err(_), Err(_)) => x.cmp(y),
            },
        };
        if o != Ordering::Equal {
            return o;
        }
    }
}

/// True when `latest` is newer than `installed`.
pub fn is_newer(latest: &str, installed: &str) -> bool {
    compare_versions(latest, installed) == Ordering::Greater
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAMPLE: &str = r#"{"version":"1.9.0","boards":[
      {"env":"waveshare_amoled_216","chip":"esp32s3","flash_size":"16MB","display_name":"AMOLED 2.16","file":"waveshare_amoled_216-v1.9.0.bin","sha256":"aa"},
      {"env":"waveshare_amoled_18","chip":"ESP32-S3","flash_size":16,"display_name":"AMOLED 1.8","file":"waveshare_amoled_18-v1.9.0.bin","sha256":"bb"},
      {"env":"waveshare_amoled_206","chip":"esp32s3","flash_size":"32MB","display_name":"AMOLED 2.06","file":"waveshare_amoled_206-v1.9.0.bin","sha256":"cc"},
      {"env":"waveshare_amoled_216_c6","chip":"esp32c6","flash_size":"16MB","display_name":"C6 2.16","file":"c6.bin","sha256":"dd"},
      {"env":"waveshare_lcd_154","chip":"esp32s3","flash_size":"","display_name":"LCD 1.54","file":"lcd.bin","sha256":"ee"}
    ]}"#;

    #[test]
    fn parse_and_lookup() {
        let m = Manifest::parse(SAMPLE).unwrap();
        assert_eq!(m.version, "1.9.0");
        assert_eq!(m.board("waveshare_amoled_206").unwrap().flash_mb(), Some(32));
        assert_eq!(m.board("waveshare_amoled_18").unwrap().flash_size, "16");
        assert!(m.board("sim").is_none());
        // Round-trips with the same field names CI writes.
        let back = serde_json::to_value(&m).unwrap();
        let keys: Vec<_> = back["boards"][0].as_object().unwrap().keys().cloned().collect();
        let mut want = vec!["chip", "display_name", "env", "file", "flash_size", "sha256"];
        want.sort();
        let mut keys = keys;
        keys.sort();
        assert_eq!(keys, want);
    }

    #[test]
    fn candidate_narrowing() {
        let m = Manifest::parse(SAMPLE).unwrap();
        let envs = |c: Vec<&BoardImage>| c.into_iter().map(|b| b.env.clone()).collect::<Vec<String>>();
        // 32 MB: the exact match first, then the 16 MB images, then unknown.
        assert_eq!(
            envs(m.candidates("ESP32-S3", Some(32))),
            ["waveshare_amoled_206", "waveshare_amoled_216", "waveshare_amoled_18", "waveshare_lcd_154"]
        );
        assert_eq!(
            envs(m.candidates("esp32s3", Some(16))),
            ["waveshare_amoled_216", "waveshare_amoled_18", "waveshare_lcd_154"]
        );
        assert_eq!(envs(m.candidates("esp32s3", Some(8))), ["waveshare_lcd_154"], "16 MB images never offered to 8 MB");
        assert_eq!(envs(m.candidates("esp32c6", None)), ["waveshare_amoled_216_c6"]);
    }

    #[test]
    fn larger_flash_still_gets_smaller_images() {
        // The original 2.16 env assumes 8 MB; some units ship with 16 MB.
        let m = Manifest::parse(
            r#"{"version":"2.0.0","boards":[
              {"env":"waveshare_amoled_216","chip":"esp32s3","flash_size":"8MB","display_name":"2.16","file":"a","sha256":""},
              {"env":"waveshare_amoled_18","chip":"esp32s3","flash_size":"16MB","display_name":"1.8","file":"b","sha256":""},
              {"env":"tiny","chip":"esp32s3","flash_size":"4MB","display_name":"tiny","file":"c","sha256":""}]}"#,
        )
        .unwrap();
        let envs = |c: Vec<&BoardImage>| c.into_iter().map(|b| b.env.clone()).collect::<Vec<String>>();
        assert_eq!(envs(m.candidates("esp32s3", Some(16))), ["waveshare_amoled_18", "waveshare_amoled_216", "tiny"]);
        assert_eq!(envs(m.candidates("esp32s3", Some(8))), ["waveshare_amoled_216", "tiny"]);
        assert_eq!(envs(m.candidates("esp32s3", Some(4))), ["tiny"]);
        assert!(m.candidates("esp32s3", Some(2)).is_empty());
    }

    #[test]
    fn sizes_and_chips() {
        assert_eq!(flash_mb("16MB"), Some(16));
        assert_eq!(flash_mb("16 MB"), Some(16));
        assert_eq!(flash_mb("32"), Some(32));
        assert_eq!(flash_mb("16777216"), Some(16));
        assert_eq!(flash_mb("4096KB"), Some(4));
        assert_eq!(flash_mb(""), None);
        assert_eq!(flash_mb("big"), None);
        assert_eq!(normalize_chip("ESP32-C6"), "esp32c6");
    }

    #[test]
    fn versions() {
        assert!(is_newer("1.10.0", "1.9.9"));
        assert!(is_newer("v1.9.1", "1.9.0"));
        assert!(!is_newer("1.9.0", "v1.9.0"));
        assert!(is_newer("1.9.0", "1.9.0-rc1"));
        assert!(!is_newer("1.9.0-rc1", "1.9.0"));
        assert!(is_newer("2", "1.99.99"));
        assert!(is_newer("2.0.0-rc.10", "2.0.0-rc.9"));
        assert!(is_newer("2.0.0-rc.2", "v2.0.0-rc.1"));
        assert!(!is_newer("2.0.0-rc.2", "2.0.0-rc.2"));
        assert!(is_newer("2.0.0-rc.1", "2.0.0-beta.3"));
        assert!(is_newer("2.0.0-rc.1.1", "2.0.0-rc.1"));
    }
}
