//! The JSON written to the device's RX characteristic.
//!
//! Keys match `parse_json()` in `firmware/src/main.cpp`, in the order the old
//! daemons wrote them, plus the additive fast-clock fields `a`, `e`, `es`,
//! `n`, `rp` (old firmware ignores unknown keys).

use chrono::{DateTime, Datelike, Months, TimeZone, Utc};
use serde::Serialize;

use crate::config::ClockMode;
use crate::usage::{SpendPeriod, UsageReading};

/// One device payload. `None` fields are omitted.
#[derive(Debug, Clone, Default, PartialEq, Serialize)]
pub struct Payload {
    /// Session % used.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub s: Option<u32>,
    /// Minutes until the session resets; -1 = window not started.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub sr: Option<i64>,
    /// Weekly % used.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub w: Option<u32>,
    /// Minutes until the weekly reset; -1 = unknown.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub wr: Option<i64>,
    /// Status: "allowed", "limited" (a window or the spend cap is used up)
    /// or "overage" (extra usage is paying). Stored, not shown, by the firmware.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub st: Option<String>,
    /// Account kind: "pro" (5h/7d windows) or "ent" (spending limit).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub acct: Option<String>,
    /// Enterprise: % of the billing period elapsed.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub tp: Option<u32>,
    /// Enterprise: billing period length in days.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub pd: Option<u32>,
    /// Enterprise: reset date text, e.g. "Jul 1".
    #[serde(skip_serializing_if = "Option::is_none")]
    pub rd: Option<String>,
    /// Local wall-clock epoch (UTC epoch shifted by the local offset).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub t: Option<i64>,
    /// Hour format, 12 or 24.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub tf: Option<u8>,
    /// 1 = the session-reset chime may sound.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub c: Option<u8>,
    /// Data is valid. `false` alone means "no data" (e.g. logged out).
    pub ok: bool,
    /// 1 = beep for this alert.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub b: Option<u8>,
    /// Attention message (ASCII).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub m: Option<String>,
    /// 1 = end the alert.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub x: Option<u8>,
    /// Activity level 0-3 (PostToolUse per minute).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub a: Option<u8>,
    /// Latest event: "p" prompt, "s" stop, "b" back after a gap.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub e: Option<String>,
    /// Sequence number of `e`, so the device reacts once.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub es: Option<u32>,
    /// Sessions with a hook event in the last 5 minutes.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub n: Option<u32>,
    /// 1 = heartbeat/replay (no fresh poll): the rate math ignores it.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub rp: Option<u8>,
}

/// Every key [`Payload`] can emit, in serialization order.
pub const PAYLOAD_KEYS: &[&str] = &[
    "s", "sr", "w", "wr", "st", "acct", "tp", "pd", "rd", "t", "tf", "c", "ok", "b", "m", "x", "a", "e", "es", "n",
    "rp",
];
/// The fast-clock keys added in v0.4 (old firmware ignores them).
pub const ADDITIVE_KEYS: &[&str] = &["a", "e", "es", "n", "rp"];

fn minutes_until(at: DateTime<Utc>, now: DateTime<Utc>) -> i64 {
    // Round like the bash daemon's sprintf("%.0f"), clamp at 0.
    let secs = (at - now).num_seconds();
    if secs <= 0 {
        0
    } else {
        (secs + 30) / 60
    }
}

/// Wall-clock fields for the `clock` setting.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ClockFields {
    /// Local epoch (`t`).
    pub local_epoch: i64,
    /// 12 or 24 (`tf`).
    pub hour_format: u8,
}

/// Compute `t`/`tf` for `mode` (None when the clock is off).
pub fn clock_fields(
    mode: ClockMode,
    now: DateTime<Utc>,
    utc_offset_secs: i64,
    locale_12h: bool,
) -> Option<ClockFields> {
    let hour_format = match mode {
        ClockMode::Off => return None,
        ClockMode::H12 => 12,
        ClockMode::H24 => 24,
        ClockMode::Auto => {
            if locale_12h {
                12
            } else {
                24
            }
        }
    };
    Some(ClockFields { local_epoch: now.timestamp() + utc_offset_secs, hour_format })
}

/// Billing-period fields for a spend-capped account.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BillingPeriod {
    /// `tp`: % of the period elapsed (0-100).
    pub elapsed_pct: u32,
    /// `pd`: period length in days.
    pub days: u32,
    /// `rd`: reset date as "Nov 1" (host-local), empty when unknown.
    pub reset_date: String,
}

impl BillingPeriod {
    /// What the old daemons sent when the reset was missing or garbage.
    pub fn unknown() -> Self {
        BillingPeriod { elapsed_pct: 0, days: 30, reset_date: String::new() }
    }
}

/// `tp` / `pd` / `rd` for a period ending at `reset`, like the old daemons'
/// `_billing_period_info`: a monthly period starts on the same day and time
/// one calendar month earlier (clamped to the month's length), in `tz`.
/// Weekly and daily periods are 7 days and 1 day.
pub fn billing_period<Tz: TimeZone>(
    reset: Option<DateTime<Utc>>,
    period: SpendPeriod,
    tz: &Tz,
    now: DateTime<Utc>,
) -> BillingPeriod
where
    Tz::Offset: std::fmt::Display,
{
    let Some(end_utc) = reset else { return BillingPeriod::unknown() };
    let end = end_utc.with_timezone(tz);
    let start_utc = match period {
        SpendPeriod::Daily => Some(end_utc - chrono::Duration::days(1)),
        SpendPeriod::Weekly => Some(end_utc - chrono::Duration::days(7)),
        SpendPeriod::Monthly => end
            .naive_local()
            .checked_sub_months(Months::new(1))
            .and_then(|n| tz.from_local_datetime(&n).earliest())
            .map(|d| d.with_timezone(&Utc)),
    };
    let Some(start_utc) = start_utc else { return BillingPeriod::unknown() };
    let len = (end_utc - start_utc).num_seconds();
    if len <= 0 {
        return BillingPeriod::unknown();
    }
    let elapsed = (now - start_utc).num_seconds() as f64 / len as f64 * 100.0;
    BillingPeriod {
        elapsed_pct: elapsed.round().clamp(0.0, 100.0) as u32,
        days: (len as f64 / 86_400.0).round() as u32,
        reset_date: format!("{} {}", end.format("%b"), end.day()),
    }
}

impl Payload {
    /// The usage part for a reading, as of `now`. Reset countdowns are
    /// recomputed from the absolute reset time, so a replay is exact. The
    /// billing-period date is in the host's local zone.
    pub fn from_reading(r: &UsageReading, now: DateTime<Utc>) -> Self {
        Self::from_reading_in(r, now, &chrono::Local)
    }

    /// [`Payload::from_reading`] with an explicit zone for `rd` / `tp`.
    pub fn from_reading_in<Tz: TimeZone>(r: &UsageReading, now: DateTime<Utc>, tz: &Tz) -> Self
    where
        Tz::Offset: std::fmt::Display,
    {
        let st = if r.overage {
            "overage"
        } else if r.limited() {
            "limited"
        } else {
            "allowed"
        };
        if let (true, Some(sp)) = (r.is_spend_only(), &r.spend) {
            // The old daemons' "ent" payload: s is the spend %, the weekly
            // meter becomes the billing-period box.
            let bp = billing_period(sp.resets_at, sp.period.unwrap_or(SpendPeriod::Monthly), tz, now);
            return Payload {
                s: Some(sp.pct),
                sr: Some(sp.resets_at.map_or(0, |t| minutes_until(t, now))),
                w: Some(0),
                wr: Some(0),
                st: Some(st.to_owned()),
                acct: Some("ent".to_owned()),
                tp: Some(bp.elapsed_pct),
                pd: Some(bp.days),
                rd: Some(bp.reset_date),
                ok: true,
                ..Default::default()
            };
        }
        let (s, sr) = match &r.session {
            Some(w) => (w.pct, w.resets_at.map_or(-1, |t| minutes_until(t, now))),
            None => (0, -1),
        };
        let (w, wr) = match &r.week {
            Some(win) => (win.pct, win.resets_at.map_or(-1, |t| minutes_until(t, now))),
            None => (0, -1),
        };
        Payload {
            s: Some(s),
            sr: Some(sr),
            w: Some(w),
            wr: Some(wr),
            st: Some(st.to_owned()),
            acct: Some("pro".to_owned()),
            ok: true,
            ..Default::default()
        }
    }

    /// The "no data" payload (every account logged out).
    pub fn no_data() -> Self {
        Payload { ok: false, ..Default::default() }
    }

    /// Age a payload by `secs` like the bash daemon's `age_payload`: positive
    /// `sr`/`wr` tick down by whole minutes (clamped at 0) and `t` advances.
    pub fn aged(&self, secs: i64) -> Self {
        let mins = secs / 60;
        let mut p = self.clone();
        for x in [&mut p.sr, &mut p.wr].into_iter().flatten() {
            if *x > 0 {
                *x = (*x - mins).max(0);
            }
        }
        if let Some(t) = &mut p.t {
            if *t > 0 {
                *t += secs;
            }
        }
        p
    }

    /// Compact JSON.
    pub fn to_json(&self) -> String {
        serde_json::to_string(self).expect("payload serializes")
    }

    /// JSON for one write of at most `limit` bytes: shorten the alert message,
    /// then drop it, until it fits (Python daemon's `encode_payload`).
    pub fn encode(&self, limit: usize) -> Vec<u8> {
        let mut data = self.to_json().into_bytes();
        let mut p = self.clone();
        let mut msg = p.m.clone().unwrap_or_default();
        while data.len() > limit && !msg.is_empty() {
            let over = data.len() - limit;
            let keep = msg.len().saturating_sub(over + 3);
            msg.truncate(keep);
            let trimmed = msg.trim_end().to_owned();
            msg = trimmed;
            p.m = Some(if msg.is_empty() { String::new() } else { format!("{msg}...") });
            data = p.to_json().into_bytes();
        }
        if data.len() > limit {
            p.m = None;
            data = p.to_json().into_bytes();
        }
        data
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::usage::Window;
    use std::collections::BTreeSet;

    fn at(s: &str) -> DateTime<Utc> {
        DateTime::parse_from_rfc3339(s).unwrap().with_timezone(&Utc)
    }

    fn p(json: &str) -> Payload {
        let v: serde_json::Value = serde_json::from_str(json).unwrap();
        let g = |k: &str| v.get(k);
        Payload {
            s: g("s").and_then(|x| x.as_u64()).map(|x| x as u32),
            sr: g("sr").and_then(|x| x.as_i64()),
            w: g("w").and_then(|x| x.as_u64()).map(|x| x as u32),
            wr: g("wr").and_then(|x| x.as_i64()),
            st: g("st").and_then(|x| x.as_str()).map(Into::into),
            acct: g("acct").and_then(|x| x.as_str()).map(Into::into),
            tp: g("tp").and_then(|x| x.as_u64()).map(|x| x as u32),
            pd: g("pd").and_then(|x| x.as_u64()).map(|x| x as u32),
            rd: g("rd").and_then(|x| x.as_str()).map(Into::into),
            t: g("t").and_then(|x| x.as_i64()),
            tf: g("tf").and_then(|x| x.as_u64()).map(|x| x as u8),
            c: g("c").and_then(|x| x.as_u64()).map(|x| x as u8),
            ok: g("ok").and_then(|x| x.as_bool()).unwrap_or(false),
            ..Default::default()
        }
    }

    // Ported from the legacy daemon test test_bash_heartbeat.sh (payload aging).
    #[test]
    fn aging_like_bash() {
        assert_eq!(
            p(r#"{"s":13,"sr":73,"w":21,"wr":6913,"st":"allowed","acct":"pro","ok":true}"#).aged(300).to_json(),
            r#"{"s":13,"sr":68,"w":21,"wr":6908,"st":"allowed","acct":"pro","ok":true}"#
        );
        assert_eq!(
            p(r#"{"s":13,"sr":73,"w":21,"wr":6913,"st":"allowed","ok":true}"#).aged(59).to_json(),
            r#"{"s":13,"sr":73,"w":21,"wr":6913,"st":"allowed","ok":true}"#
        );
        assert_eq!(
            p(r#"{"s":13,"sr":2,"w":21,"wr":0,"st":"allowed","c":1,"t":1758300000,"tf":12,"ok":true}"#)
                .aged(245)
                .to_json(),
            r#"{"s":13,"sr":0,"w":21,"wr":0,"st":"allowed","t":1758300245,"tf":12,"c":1,"ok":true}"#
        );
        assert_eq!(
            p(r#"{"s":99,"sr":61,"w":0,"wr":0,"st":"allowed","acct":"ent","tp":40,"pd":30,"rd":"Oct 1","ok":true}"#)
                .aged(60)
                .to_json(),
            r#"{"s":99,"sr":60,"w":0,"wr":0,"st":"allowed","acct":"ent","tp":40,"pd":30,"rd":"Oct 1","ok":true}"#
        );
        // Not started (-1) is never aged.
        assert_eq!(p(r#"{"sr":-1,"ok":true}"#).aged(600).sr, Some(-1));
    }

    #[test]
    fn from_reading() {
        let now = at("2026-10-13T05:40:00Z");
        let r = UsageReading {
            session: Some(Window { pct: 15, resets_at: Some(at("2026-10-13T07:00:00Z")) }),
            week: Some(Window { pct: 6, resets_at: Some(at("2026-10-18T22:00:00Z")) }),
            ..Default::default()
        };
        assert_eq!(
            Payload::from_reading(&r, now).to_json(),
            r#"{"s":15,"sr":80,"w":6,"wr":8180,"st":"allowed","acct":"pro","ok":true}"#
        );
        let not_started = UsageReading { session: None, ..r.clone() };
        let pl = Payload::from_reading(&not_started, now);
        assert_eq!((pl.s, pl.sr), (Some(0), Some(-1)));
        // Rounds to the nearest minute, clamps at 0.
        let soon = UsageReading {
            session: Some(Window { pct: 100, resets_at: Some(at("2026-10-13T05:40:31Z")) }),
            ..Default::default()
        };
        let pl = Payload::from_reading(&soon, now);
        assert_eq!((pl.sr, pl.st.as_deref()), (Some(1), Some("limited")));
        assert_eq!(Payload::from_reading(&soon, at("2026-10-13T06:00:00Z")).sr, Some(0));
        assert_eq!(Payload::no_data().to_json(), r#"{"ok":false}"#);
    }

    // Ported from the enterprise branch of the old daemons
    // (`_billing_period_info`, test_windows_poll.py's guards).
    #[test]
    fn billing_period_like_the_old_daemons() {
        use chrono_tz::Tz;
        let utc: Tz = "UTC".parse().unwrap();
        let now = at("2026-10-16T00:00:00Z");
        // Oct 1 -> Nov 1: 31 days, 15 elapsed.
        let bp = billing_period(Some(at("2026-11-01T00:00:00Z")), SpendPeriod::Monthly, &utc, now);
        assert_eq!(bp, BillingPeriod { elapsed_pct: 48, days: 31, reset_date: "Nov 1".into() });
        // Mar 31 reset: the period starts Feb 28 (clamped), like calendar.monthrange.
        let bp =
            billing_period(Some(at("2027-03-31T00:00:00Z")), SpendPeriod::Monthly, &utc, at("2027-03-01T00:00:00Z"));
        assert_eq!((bp.days, bp.reset_date.as_str()), (31, "Mar 31"));
        assert_eq!(bp.elapsed_pct, 3);
        // January reset steps back into December of the previous year.
        let bp = billing_period(Some(at("2027-01-15T00:00:00Z")), SpendPeriod::Monthly, &utc, now);
        assert_eq!((bp.days, bp.elapsed_pct), (31, 0), "before the period: clamped at 0");
        // After the reset: clamped at 100.
        let bp = billing_period(Some(at("2026-10-10T00:00:00Z")), SpendPeriod::Weekly, &utc, now);
        assert_eq!((bp.days, bp.elapsed_pct), (7, 100));
        // The date is host-local: midnight UTC is still Oct 31 in New York.
        let ny: Tz = "America/New_York".parse().unwrap();
        let bp = billing_period(Some(at("2026-11-01T00:00:00Z")), SpendPeriod::Monthly, &ny, now);
        assert_eq!(bp.reset_date, "Oct 31");
        // No reset: the old daemons' neutral default.
        assert_eq!(billing_period(None, SpendPeriod::Monthly, &utc, now), BillingPeriod::unknown());
    }

    #[test]
    fn spend_only_reading_is_the_enterprise_payload() {
        use crate::usage::Spend;
        let utc: chrono_tz::Tz = "UTC".parse().unwrap();
        let now = at("2026-10-16T00:00:00Z");
        let r = UsageReading {
            spend: Some(Spend { pct: 41, resets_at: Some(at("2026-11-01T00:00:00Z")), period: None }),
            ..Default::default()
        };
        assert_eq!(
            Payload::from_reading_in(&r, now, &utc).to_json(),
            r#"{"s":41,"sr":23040,"w":0,"wr":0,"st":"allowed","acct":"ent","tp":48,"pd":31,"rd":"Nov 1","ok":true}"#
        );
        // Session/week lines win over a spend line (subscription view).
        let both = UsageReading { session: Some(Window { pct: 5, resets_at: None }), ..r.clone() };
        assert_eq!(Payload::from_reading_in(&both, now, &utc).acct.as_deref(), Some("pro"));
        // Over the cap, no reset known.
        let over =
            UsageReading { spend: Some(Spend { pct: 104, resets_at: None, period: None }), ..Default::default() };
        let p = Payload::from_reading_in(&over, now, &utc);
        assert_eq!(
            (p.st.as_deref(), p.sr, p.tp, p.pd, p.rd.as_deref()),
            (Some("limited"), Some(0), Some(0), Some(30), Some(""))
        );
    }

    #[test]
    fn status_string() {
        let now = at("2026-10-13T05:40:00Z");
        let w = |pct| Some(Window { pct, resets_at: None });
        let st = |r: UsageReading| Payload::from_reading(&r, now).st.unwrap();
        assert_eq!(st(UsageReading { session: w(40), week: w(99), ..Default::default() }), "allowed");
        assert_eq!(st(UsageReading { session: w(40), week: w(100), ..Default::default() }), "limited");
        assert_eq!(st(UsageReading { session: w(100), week: w(10), overage: true, ..Default::default() }), "overage");
    }

    #[test]
    fn clock() {
        let now = at("2026-10-13T12:00:00Z");
        assert_eq!(clock_fields(ClockMode::Off, now, 3600, true), None);
        let f = clock_fields(ClockMode::Auto, now, 3600, true).unwrap();
        assert_eq!((f.local_epoch, f.hour_format), (now.timestamp() + 3600, 12));
        assert_eq!(clock_fields(ClockMode::H24, now, 0, true).unwrap().hour_format, 24);
    }

    // Ported from the legacy daemon test test_attention.py::test_encode_payload_fits.
    #[test]
    fn encode_fits() {
        let base = Payload { s: Some(4), ok: true, b: Some(1), ..Default::default() };
        let short = Payload { m: Some("hi".into()), ..base.clone() }.encode(180);
        let v: serde_json::Value = serde_json::from_slice(&short).unwrap();
        assert_eq!(v["m"], "hi");
        let long = Payload { m: Some("x".repeat(200)), ..base.clone() }.encode(120);
        assert!(long.len() <= 120);
        let v: serde_json::Value = serde_json::from_slice(&long).unwrap();
        assert!(v["m"].as_str().unwrap().ends_with("..."));
        let tiny = Payload { m: Some("x".repeat(200)), ..base }.encode(26);
        let v: serde_json::Value = serde_json::from_slice(&tiny).unwrap();
        assert!(v.get("m").is_none());
    }

    #[test]
    fn full_payload_key_order() {
        let pl = Payload {
            s: Some(1),
            sr: Some(2),
            w: Some(3),
            wr: Some(4),
            st: Some("allowed".into()),
            acct: Some("pro".into()),
            tp: Some(1),
            pd: Some(30),
            rd: Some("Oct 1".into()),
            t: Some(5),
            tf: Some(24),
            c: Some(1),
            ok: true,
            b: Some(1),
            m: Some("hi".into()),
            x: Some(1),
            a: Some(2),
            e: Some("p".into()),
            es: Some(7),
            n: Some(1),
            rp: Some(1),
        };
        let v: serde_json::Map<String, serde_json::Value> = serde_json::from_str(&pl.to_json()).unwrap();
        // serde_json's Map is sorted without preserve_order; check membership.
        let got: BTreeSet<&str> = v.keys().map(String::as_str).collect();
        let want: BTreeSet<&str> = PAYLOAD_KEYS.iter().copied().collect();
        assert_eq!(got, want);
        let json = pl.to_json();
        let positions: Vec<usize> = PAYLOAD_KEYS.iter().map(|k| json.find(&format!("\"{k}\":")).unwrap()).collect();
        assert!(positions.windows(2).all(|w| w[0] < w[1]), "keys serialize in PAYLOAD_KEYS order");
    }

    /// Every key the firmware's JSON parser reads is one we can send, and
    /// every key we send is either read by the firmware or one of the additive
    /// fast-clock keys. Scans every shared `firmware/src/*.cpp` (the parser
    /// lives in `payload.cpp`; older trees had it in `main.cpp`).
    #[test]
    fn keys_match_firmware_parser() {
        let src_dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../firmware/src");
        let Ok(rd) = std::fs::read_dir(&src_dir) else {
            eprintln!("firmware source not found at {}; skipping", src_dir.display());
            return;
        };
        let mut fw = BTreeSet::new();
        for entry in rd.flatten() {
            let path = entry.path();
            if path.extension().is_none_or(|e| e != "cpp") {
                continue;
            }
            let src = std::fs::read_to_string(&path).unwrap_or_default();
            for part in src.split("doc[\"").skip(1) {
                if let Some(end) = part.find('"') {
                    fw.insert(part[..end].to_owned());
                }
            }
        }
        assert!(fw.contains("s") && fw.contains("ok"), "parser keys found: {fw:?}");
        let ours: BTreeSet<String> = PAYLOAD_KEYS.iter().map(|s| s.to_string()).collect();
        for k in &fw {
            assert!(ours.contains(k), "firmware reads {k:?}, which the payload never sends");
        }
        for k in &ours {
            assert!(
                fw.contains(k) || ADDITIVE_KEYS.contains(&k.as_str()),
                "payload sends {k:?}, which the firmware doesn't read"
            );
        }
    }
}
