//! Renders the macOS tray menu's header for a few made-up states, in light
//! and dark mode, as PNGs (the header is drawn by AppKit, so this is the way
//! to review it without opening the menu). First it puts the panel into a
//! muda menu the way the tray does, appends a few items after it, and checks
//! the NSMenu came out as the panel, then those items.
//!
//! `cargo run -p clawdmeter-app --example tray_header -- <out dir>`

#[cfg(target_os = "macos")]
fn main() {
    use clawdmeter_app::ipc::StatusSnapshot;
    use clawdmeter_app::tray::header::Header;
    use clawdmeter_app::tray::mac_menu::MenuHeader;

    let out = std::path::PathBuf::from(std::env::args().nth(1).unwrap_or_else(|| ".".into()));
    std::fs::create_dir_all(&out).expect("out dir");
    let mtm = objc2::MainThreadMarker::new().expect("main thread");
    let _app = objc2_app_kit::NSApplication::sharedApplication(mtm);
    {
        use tray_icon::menu::{CheckMenuItem, Menu, MenuItem, PredefinedMenuItem};
        let menu = Menu::new();
        let _header = MenuHeader::install(&menu);
        let (a, b, c) = (
            MenuItem::new("Refresh usage", true, None),
            CheckMenuItem::new("Chime on this computer", true, false, None),
            MenuItem::new("Quit Clawdmeter", true, None),
        );
        menu.append_items(&[&a, &b, &PredefinedMenuItem::separator(), &c]).expect("append");
        let ns = clawdmeter_app::tray::mac_menu::ns_menu(&menu);
        let item = |i| ns.itemAtIndex(i).expect("item");
        assert_eq!(ns.numberOfItems(), 5, "the panel, then the four appended");
        assert!(item(0).view().is_some(), "the header is the first item");
        assert_eq!(item(1).title().to_string(), "Refresh usage");
        assert_eq!(item(4).title().to_string(), "Quit Clawdmeter");
        println!("menu: panel, then {} items", ns.numberOfItems() - 1);
    }
    let home = std::path::Path::new("/Users/you");
    let now = clawdmeter_core::unix_now();
    let base = StatusSnapshot {
        link: "connected".into(),
        tray: "Connected".into(),
        account: Some("/Users/you/.claude".into()),
        read_at: Some(now - 360),
        ..Default::default()
    };
    let states = [
        (
            "weekly-only",
            false,
            StatusSnapshot {
                no_session_limit: true,
                week_pct: Some(57),
                week_resets_at: Some(now + (3 * 24 + 4) * 3600 + 60),
                ..base.clone()
            },
        ),
        (
            "session-weekly-pace",
            true,
            StatusSnapshot {
                session_pct: Some(34),
                window_started: true,
                session_resets_at: Some(now + 9000),
                week_pct: Some(62),
                week_resets_at: Some(now + (5 * 24 + 11) * 3600),
                ..base.clone()
            },
        ),
        (
            "near-limit",
            false,
            StatusSnapshot {
                session_pct: Some(92),
                window_started: true,
                session_resets_at: Some(now + 40 * 60),
                week_pct: Some(81),
                week_resets_at: Some(now + 26 * 3600),
                ..base.clone()
            },
        ),
        (
            "not-started",
            false,
            StatusSnapshot {
                session_pct: Some(0),
                week_pct: Some(12),
                week_resets_at: Some(now + 6 * 86400),
                ..base.clone()
            },
        ),
        (
            "codex-weekly-pace",
            true,
            StatusSnapshot {
                codex: true,
                account: Some("/Users/you/.codex".into()),
                no_session_limit: true,
                week_pct: Some(34),
                week_resets_at: Some(now + (4 * 24 + 21) * 3600),
                ..base.clone()
            },
        ),
        (
            "codex-5h",
            false,
            StatusSnapshot {
                codex: true,
                account: Some("/Users/you/.codex".into()),
                session_pct: Some(21),
                window_started: true,
                session_resets_at: Some(now + 133 * 60),
                week_pct: Some(34),
                week_resets_at: Some(now + (4 * 24 + 21) * 3600),
                ..base.clone()
            },
        ),
        (
            "long-folder",
            false,
            StatusSnapshot {
                account: Some("/Volumes/Work/claude-config/team-accounts/personal".into()),
                read_at: Some(now - 720),
                no_session_limit: true,
                week_pct: Some(57),
                week_resets_at: Some(now + (3 * 24 + 4) * 3600),
                ..base.clone()
            },
        ),
        (
            "signed-out",
            false,
            StatusSnapshot {
                tray: "AuthError".into(),
                logged_out: true,
                session_pct: Some(34),
                window_started: true,
                session_resets_at: Some(now + 9000),
                week_pct: Some(62),
                week_resets_at: Some(now + (5 * 24 + 11) * 3600),
                ..base.clone()
            },
        ),
        (
            "checking",
            false,
            StatusSnapshot {
                polling: true,
                no_session_limit: true,
                week_pct: Some(57),
                week_resets_at: Some(now + (3 * 24 + 4) * 3600),
                ..base.clone()
            },
        ),
        (
            "wait",
            false,
            StatusSnapshot {
                refresh_at: Some(now + 100),
                no_session_limit: true,
                week_pct: Some(57),
                week_resets_at: Some(now + (3 * 24 + 4) * 3600),
                ..base.clone()
            },
        ),
        (
            "paused",
            false,
            StatusSnapshot {
                paused_until: Some(now + 1800),
                no_session_limit: true,
                week_pct: Some(57),
                week_resets_at: Some(now + (3 * 24 + 4) * 3600),
                read_at: Some(now - 2400),
                ..base.clone()
            },
        ),
        (
            "update-available",
            false,
            StatusSnapshot {
                account: Some("/Users/you/.claude-personal".into()),
                read_at: Some(now - 120),
                no_session_limit: true,
                week_pct: Some(61),
                week_resets_at: Some(now + (5 * 24 + 2) * 3600),
                latest_firmware: Some("1.9.0".into()),
                device_firmware: Some("1.8.2".into()),
                ..base.clone()
            },
        ),
        (
            "chime-off",
            false,
            StatusSnapshot {
                account: Some("/Users/you/.claude-personal".into()),
                read_at: Some(now - 120),
                no_session_limit: true,
                week_pct: Some(61),
                week_resets_at: Some(now + (5 * 24 + 2) * 3600),
                ..base.clone()
            },
        ),
        (
            // The sample the debug switches show on every OS
            // (CLAWDMETER_DEMO_USAGE=1, CLAWDMETER_DEMO_FIRMWARE_UPDATE,
            // pace marker on), for side-by-side screenshots.
            "compare",
            true,
            StatusSnapshot {
                session_pct: Some(34),
                window_started: true,
                session_resets_at: Some(now + 2 * 3600 + 30 * 60),
                week_pct: Some(62),
                week_resets_at: Some(now + (5 * 24 + 11) * 3600),
                read_at: Some(now - 120),
                ..base.clone()
            },
        ),
        (
            "no-data",
            false,
            StatusSnapshot {
                link: "searching".into(),
                tray: "Searching".into(),
                account: None,
                read_at: None,
                ..Default::default()
            },
        ),
    ];
    for (name, pace, snap) in states {
        for dark in [false, true] {
            let menu = tray_icon::menu::Menu::new();
            let header = MenuHeader::install(&menu);
            let refresh = clawdmeter_app::ui::overview::refresh_state(&snap, false, now);
            // The chime on this computer: on, except in the "chime-off" state.
            let chime = name != "chime-off" && name != "compare";
            let mut h = Header::new(&snap, home, pace, chime, refresh, now);
            // The tray works out the offer (app first); here it's made up.
            if name == "update-available" {
                h.update = Some(clawdmeter_app::tray::Update::App("v1.9.0".into()));
            }
            if name == "compare" {
                h.update = Some(clawdmeter_app::tray::Update::Firmware("v1.9.0".into()));
            }
            let png = header.preview_png(&h, dark).expect("render");
            let path = out.join(format!("{name}-{}.png", if dark { "dark" } else { "light" }));
            std::fs::write(&path, png).expect("write");
            println!("{}", path.display());
        }
    }
}

#[cfg(not(target_os = "macos"))]
fn main() {
    eprintln!("The drawn tray header is macOS only.");
}
