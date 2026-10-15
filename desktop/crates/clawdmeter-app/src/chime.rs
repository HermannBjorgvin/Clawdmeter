//! The desktop chime: the device's ding (firmware `ding.h`), synthesized and
//! played through the default audio output. A ding at C#5: a sine strike
//! with three faint partials, a 0.7 ms attack, a two-stage decay (a fast 9 ms
//! fall into a slow 0.24 s ring) and a short high-passed noise click at the
//! start. Keep it in step with the firmware: the golden frames in the tests
//! are shared.

use std::f32::consts::TAU;
use std::num::NonZero;
use std::time::Duration;

/// C#5.
const F0: f32 = 554.5;
/// 3rd, 2nd and 5th partials.
const P3: f32 = 0.02;
const P2: f32 = 0.012;
const P5: f32 = 0.012;
/// Attack (s).
const ATTACK_S: f32 = 0.0007;
/// The slow ring and the fast strike decay: level, time constant (s).
const SLOW: (f32, f32) = (0.55, 0.24);
const FAST: (f32, f32) = (0.34, 0.009);
/// The click: level, time constant, how long it's computed (s).
const CLICK: (f32, f32, f32) = (0.08, 0.0015, 0.02);
/// Overall level: +0.8 dB.
const LEVEL: f32 = 1.096_478_2;
/// Length; the ring is about -65 dB by then (s).
pub const LENGTH_S: f32 = 1.67;
/// Playback rate.
pub const RATE: u32 = 44_100;
/// Playback level (of the ding's own, which peaks just under full scale).
pub const GAIN: f32 = 1.0;

/// Frames in one ding at `rate` Hz.
pub fn frames(rate: u32) -> u32 {
    (LENGTH_S * rate as f32) as u32
}

/// Unit-variance uniform noise for frame `n`, from a counter hash (fixed
/// seed): any frame can be computed alone, and every build makes the same
/// click as the device.
fn noise(n: u32) -> f32 {
    let mut h = n.wrapping_mul(0x9E37_79B9).wrapping_add(7);
    h ^= h >> 16;
    h = h.wrapping_mul(0x85EB_CA6B);
    h ^= h >> 13;
    h = h.wrapping_mul(0xC2B2_AE35);
    h ^= h >> 16;
    let u = (h >> 8) as f32 * (1.0 / 16_777_216.0);
    (u * 2.0 - 1.0) * 1.732_050_8
}

/// Frame `n` of the ding at `rate` Hz, in -1..1 (0 past the end).
pub fn value(n: u32, rate: u32) -> f32 {
    if rate == 0 || n >= frames(rate) {
        return 0.0;
    }
    let t = n as f32 / rate as f32;
    let ph = TAU * F0 * t;
    let tone = ph.sin() + P3 * (3.0 * ph).sin() + P2 * (2.0 * ph).sin() + P5 * (5.0 * ph).sin();
    let atk = (t / ATTACK_S).min(1.0);
    let mut y = atk * (SLOW.0 * (-t / SLOW.1).exp() + FAST.0 * (-t / FAST.1).exp()) * tone;
    if t < CLICK.2 {
        // White noise minus its centred 4-frame mean: a crude high-pass.
        let mean = (noise(n.wrapping_sub(2)) + noise(n.wrapping_sub(1)) + noise(n) + noise(n.wrapping_add(1))) * 0.25;
        y += CLICK.0 * (-t / CLICK.1).exp() * (noise(n) - mean);
    }
    (y * LEVEL).clamp(-1.0, 1.0)
}

/// One ding as a mono rodio source.
#[derive(Debug, Clone)]
pub struct Ding {
    n: u32,
    rate: u32,
    gain: f32,
}

impl Ding {
    /// A ding at `rate` Hz, peaking at `gain`.
    pub fn new(rate: u32, gain: f32) -> Self {
        Ding { n: 0, rate: rate.max(1), gain: gain.clamp(0.0, 1.0) }
    }
}

impl Iterator for Ding {
    type Item = rodio::Sample;

    fn next(&mut self) -> Option<Self::Item> {
        if self.n >= frames(self.rate) {
            return None;
        }
        let v = value(self.n, self.rate) * self.gain;
        self.n += 1;
        Some(v as rodio::Sample)
    }
}

impl rodio::Source for Ding {
    fn current_span_len(&self) -> Option<usize> {
        Some(frames(self.rate).saturating_sub(self.n) as usize)
    }

    fn channels(&self) -> rodio::ChannelCount {
        NonZero::<u16>::MIN
    }

    fn sample_rate(&self) -> rodio::SampleRate {
        NonZero::new(self.rate).unwrap_or(NonZero::<u32>::MIN)
    }

    fn total_duration(&self) -> Option<Duration> {
        Some(Duration::from_secs_f32(LENGTH_S))
    }
}

/// Play one ding on its own thread. No audio output: logged, nothing else.
pub fn play() {
    let spawned = std::thread::Builder::new().name("chime".into()).spawn(|| {
        match rodio::DeviceSinkBuilder::open_default_sink() {
            Ok(mut sink) => {
                sink.log_on_drop(false);
                let player = rodio::Player::connect_new(sink.mixer());
                player.append(Ding::new(RATE, GAIN));
                player.sleep_until_end();
            }
            Err(e) => log::warn!("chime: no audio output ({e})"),
        }
    });
    if let Err(e) = spawned {
        log::warn!("chime: {e}");
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rodio::Source;

    fn peak(a: u32, b: u32) -> f32 {
        (a..b).map(|n| value(n, RATE).abs()).fold(0.0, f32::max)
    }

    #[test]
    fn the_ding_matches_the_device() {
        assert_eq!(frames(RATE), 73_647);
        assert_eq!(value(frames(RATE), RATE), 0.0);
        let p = peak(0, RATE / 10);
        assert!(p > 0.9 && p < 1.0, "{p}");
        assert!(peak(0, 10) < 0.2, "sub-millisecond attack");
        let period = (RATE as f32 / F0) as u32 + 1;
        let (t, tau) = (RATE / 10, (0.24 * RATE as f32) as u32);
        let ratio = peak(t + tau, t + tau + period) / peak(t, t + period);
        assert!((ratio - (-1.0f32).exp()).abs() < 0.03, "{ratio}");
        let rising =
            (RATE / 50..RATE / 50 + RATE / 2).filter(|&n| value(n - 1, RATE) < 0.0 && value(n, RATE) >= 0.0).count();
        assert!((275..=279).contains(&rising), "{rising}");
        assert!(peak(frames(RATE) - 200, frames(RATE)) < 0.001);
        // The firmware's golden frames (test/test_ding): the same sound.
        for (n, v) in [
            (20, 0.588196),
            (100, 0.892679),
            (441, -0.209996),
            (1000, -0.269436),
            (4410, 0.131246),
            (10_000, -0.231227),
        ] {
            assert!((value(n, RATE) - v).abs() < 2e-4, "frame {n}: {}", value(n, RATE));
        }
    }

    #[test]
    fn the_source_is_one_mono_ding() {
        let d = Ding::new(RATE, 0.5);
        assert_eq!((d.channels().get(), d.sample_rate().get()), (1, RATE));
        assert_eq!(d.total_duration(), Some(Duration::from_secs_f32(LENGTH_S)));
        let samples: Vec<rodio::Sample> = d.collect();
        assert_eq!(samples.len(), frames(RATE) as usize);
        assert!(samples.iter().all(|s| s.abs() <= 0.5));
        assert!(Ding::new(RATE, 3.0).all(|s| s.abs() <= 1.0), "gain clamps");
    }
}
