//! Pins the `/usage` parser against captured CLI output (`max.json`,
//! `work.json`, trimmed of the "What's contributing" section) and synthesized
//! variants for other plans and formats. The spend-limit, overage and
//! next-year fixtures follow the strings Claude Code 2.1.296 builds in its
//! `/usage` command.

use chrono::{DateTime, Utc};
use clawdmeter_core::payload::Payload;
use clawdmeter_core::usage::{parse_cli_output, UsageError, UsageReading};

fn at(s: &str) -> DateTime<Utc> {
    DateTime::parse_from_rfc3339(s).unwrap().with_timezone(&Utc)
}

/// When the real captures were taken (Reykjavik is UTC+0).
fn now() -> DateTime<Utc> {
    at("2026-10-13T05:40:00Z")
}

fn fixture(name: &str) -> String {
    let p = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures").join(name);
    std::fs::read_to_string(&p).unwrap_or_else(|e| panic!("{}: {e}", p.display()))
}

fn parse(name: &str) -> Result<UsageReading, UsageError> {
    parse_cli_output(&fixture(name), now())
}

#[test]
fn captured_max_plan() {
    let r = parse("max.json").unwrap();
    let s = r.session.as_ref().unwrap();
    assert_eq!((s.pct, s.resets_at), (15, Some(at("2026-10-13T07:00:00Z"))));
    let w = r.week.as_ref().unwrap();
    assert_eq!((w.pct, w.resets_at), (6, Some(at("2026-10-18T22:00:00Z"))));
    assert_eq!(r.models.len(), 1);
    assert_eq!(r.models[0].0, "Fable");
    assert_eq!(r.models[0].1.pct, 0);
    assert!(r.window_started());
    assert_eq!(
        Payload::from_reading(&r, now()).to_json(),
        r#"{"s":15,"sr":80,"w":6,"wr":8180,"st":"allowed","acct":"pro","ok":true}"#
    );
}

#[test]
fn captured_work_plan() {
    let r = parse("work.json").unwrap();
    let s = r.session.as_ref().unwrap();
    assert_eq!((s.pct, s.resets_at), (0, Some(at("2026-10-13T09:59:00Z"))));
    let w = r.week.as_ref().unwrap();
    assert_eq!((w.pct, w.resets_at), (38, Some(at("2026-10-14T14:59:00Z"))));
}

#[test]
fn synthesized_pro() {
    let r = parse("pro.json").unwrap();
    // 3:30pm PDT = 22:30Z.
    assert_eq!(r.session.unwrap().resets_at, Some(at("2026-10-13T22:30:00Z")));
    assert_eq!(r.week.unwrap().resets_at, Some(at("2026-10-16T16:00:00Z")));
    assert!(r.models.is_empty());
}

#[test]
fn synthesized_team_with_model_lines() {
    let r = parse("team.json").unwrap();
    // 12pm BST = 11:00Z; 12:05am BST = 23:05Z the day before.
    assert_eq!(r.session.unwrap().resets_at, Some(at("2026-10-13T11:00:00Z")));
    assert_eq!(r.week.unwrap().resets_at, Some(at("2026-10-14T23:05:00Z")));
    let names: Vec<_> = r.models.iter().map(|(n, w)| (n.as_str(), w.pct)).collect();
    assert_eq!(names, [("Opus", 61), ("Sonnet", 3)]);
}

#[test]
fn window_not_started() {
    for f in ["not-started.json", "session-no-reset.json"] {
        let r = parse(f).unwrap();
        assert!(!r.window_started(), "{f}");
        let p = Payload::from_reading(&r, now());
        assert_eq!((p.s, p.sr, p.w), (Some(0), Some(-1), Some(38)), "{f}");
    }
}

#[test]
fn enterprise_like_week_only() {
    let r = parse("enterprise-week-only.json").unwrap();
    assert!(r.session.is_none());
    assert_eq!(r.week.unwrap().resets_at, Some(at("2026-11-01T00:00:00Z")));
}

#[test]
fn enterprise_spend_limit() {
    let utc: chrono_tz::Tz = "UTC".parse().unwrap();
    let r = parse("enterprise-spend.json").unwrap();
    assert!(r.is_spend_only());
    assert_eq!(
        Payload::from_reading_in(&r, now(), &utc).to_json(),
        r#"{"s":21,"sr":27020,"w":0,"wr":0,"st":"allowed","acct":"ent","tp":39,"pd":31,"rd":"Nov 1","ok":true}"#
    );
    let r = parse("enterprise-spend-pct.json").unwrap();
    let p = Payload::from_reading_in(&r, now(), &utc);
    assert_eq!((p.s, p.st.as_deref(), p.acct.as_deref()), (Some(104), Some("limited"), Some("ent")));
}

#[test]
fn overage_status() {
    let r = parse("overage.json").unwrap();
    assert!(r.overage);
    let p = Payload::from_reading(&r, now());
    assert_eq!((p.s, p.st.as_deref(), p.acct.as_deref()), (Some(100), Some("overage"), Some("pro")));
}

#[test]
fn reset_in_another_year() {
    let r = parse("year-rollover.json").unwrap();
    assert_eq!(r.week.as_ref().unwrap().resets_at, Some(at("2027-01-02T21:59:00Z")));
    assert_eq!(Payload::from_reading(&r, now()).st.as_deref(), Some("limited"));
}

#[test]
fn twelve_hour_formats_and_rounding() {
    let r = parse("formats-12h.json").unwrap();
    let s = r.session.unwrap();
    // 99.6 rounds to 100; 7 PM JST = 10:00Z.
    assert_eq!((s.pct, s.resets_at), (100, Some(at("2026-10-13T10:00:00Z"))));
    // 12:00 a.m. JST Oct 19 = 15:00Z Oct 18.
    assert_eq!(r.week.unwrap().resets_at, Some(at("2026-10-18T15:00:00Z")));
}

#[test]
fn failures_are_errors_not_zeroes() {
    assert!(matches!(parse("garbage.json"), Err(UsageError::NoUsageLines(_))));
    assert!(matches!(parse("logged-out.json"), Err(UsageError::NotLoggedIn(_))));
    assert!(matches!(parse("cli-error.json"), Err(UsageError::BadOutput(_))));
    assert!(matches!(parse_cli_output("", now()), Err(UsageError::BadOutput(_))));
    assert!(matches!(parse_cli_output("{not json", now()), Err(UsageError::BadOutput(_))));
    assert!(matches!(
        parse_cli_output(r#"{"result":"You are currently using your API key"}"#, now()),
        Err(UsageError::NotSubscription)
    ));
}

#[test]
fn noise_before_json_is_tolerated() {
    let out = format!("warning: something\n{}", fixture("max.json"));
    assert_eq!(parse_cli_output(&out, now()).unwrap().session.unwrap().pct, 15);
}
