//! The settings pages: Device, Accounts, Hooks and Settings,
//! one view behind four sidebar entries. The setup wizard borrows the Hooks
//! page.
//!
//! It edits the same config file the daemon reads (through
//! `ConfigFile`/`Settings`, which keep comments, unknown keys and hand-written
//! values), so a hand-edited config and the GUI never disagree. The daemon
//! re-reads the file every few seconds.

use std::path::{Path, PathBuf};
use std::time::Duration;

use clawdmeter_core::autostart::{self, LaunchMode};
use clawdmeter_core::config::{AlertOn, ClockMode, ConfigFile};
use clawdmeter_core::{Paths, Settings};
use clawdmeter_flash::device::{self, FirmwareInfo};
use gpui::{div, img, prelude::*, px, Context, EventEmitter, PathPromptOptions, SharedString, Window};

use super::theme::{Theme, FONT_DISPLAY, FONT_MONO, R_M, R_S};
use super::widgets::{
    button, caption, card, checkbox, field, field_stacked, heading, icon, notice, para, pill, segmented, subheading,
    toggle, ButtonKind, Tone,
};
use super::{command, live_status, not_running_text, run_blocking, Navigate, REFRESH};
use crate::diff::{line_diff, with_context, DiffLine};
use crate::errors;
use crate::hooks::install::{self as hooks, HookPlan, HookStatus, HOOKS};
use crate::ipc::{self, AppCommand, FirmwareRecord, StatusSnapshot};
use crate::tilde;
use crate::{boards, Page};

struct HookRow {
    dir: PathBuf,
    status: HookStatus,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum HookAction {
    Install,
    Remove,
}

struct HookPreview {
    action: HookAction,
    plans: Vec<HookPlan>,
}

/// How long "Sent" stays on the test alert button.
const TEST_ALERT_SENT: Duration = Duration::from_secs(3);
/// How long a test alert failure stays beside the button.
const TEST_ALERT_FAILED: Duration = Duration::from_secs(8);

#[derive(Default)]
struct DeviceState {
    probing: bool,
    port: Option<String>,
    info: Option<FirmwareInfo>,
    busy: bool,
    /// Result of clearing the board's pairing over USB.
    unpair: Option<(Tone, String)>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum PollChoice {
    Auto,
    Every(u64),
}

/// The settings pages.
pub struct SettingsView {
    paths: Paths,
    page: Page,
    /// Shown inside the setup wizard (no page heading).
    embedded: bool,
    /// One of this view's pages is on screen.
    visible: bool,
    snap: Option<StatusSnapshot>,
    settings: Settings,
    config_error: Option<String>,
    hooks: Vec<HookRow>,
    preview: Option<HookPreview>,
    hook_msg: Option<(Tone, String)>,
    /// The last test alert's outcome, shown on and beside its button for a
    /// few seconds. `Err` holds the reason it didn't reach the app.
    test_alert: Option<Result<(), String>>,
    /// Bumped per test alert so an older reset timer leaves a newer result.
    test_alert_seq: u32,
    /// The binary the login entry launches (`None`: couldn't be
    /// determined). See [`clawdmeter_core::launch`].
    launch_exe: Option<PathBuf>,
    /// The binary the hooks run: the same, except an AppImage's copy under
    /// the state dir.
    hook_exe: Option<PathBuf>,
    device: DeviceState,
    /// Probe USB for a board (off in tests).
    usb: bool,
    autostart_on: bool,
    /// The login entry launches something other than [`Self::launch_exe`]
    /// (gone, or another copy): what it launches, if readable.
    autostart_stale: Option<Option<PathBuf>>,
    /// The old daemon's login entry (systemd unit, LaunchAgent or Run value).
    legacy: Option<autostart::LegacyEntry>,
    notice: Option<(Tone, String)>,
    /// The Settings page's Advanced section is open.
    advanced: bool,
    ticks: u32,
}

impl EventEmitter<Navigate> for SettingsView {}

impl SettingsView {
    /// Build and load the config. USB is probed while the Device page is up.
    pub fn new(paths: Paths, page: Page, cx: &mut Context<Self>) -> Self {
        Self::with_usb(paths, page, true, cx)
    }

    /// [`SettingsView::new`]; `usb: false` never touches serial ports (tests).
    pub fn with_usb(paths: Paths, page: Page, usb: bool, cx: &mut Context<Self>) -> Self {
        cx.spawn(async move |this, cx| loop {
            cx.background_executor().timer(REFRESH).await;
            if this.update(cx, |v, cx| v.tick(cx)).is_err() {
                break;
            }
        })
        .detach();
        let hook_exe = hooks::hook_binary(&paths.state_dir).ok();
        let mut v = SettingsView {
            settings: Settings::from_config(&ConfigFile::default(), &paths, None),
            paths,
            page,
            embedded: false,
            visible: false,
            snap: None,
            config_error: None,
            hooks: Vec::new(),
            preview: None,
            hook_msg: None,
            test_alert: None,
            test_alert_seq: 0,
            launch_exe: clawdmeter_core::launch::stable_launch_path().ok(),
            hook_exe,
            device: DeviceState::default(),
            usb,
            autostart_on: false,
            autostart_stale: None,
            legacy: None,
            notice: None,
            advanced: false,
            ticks: 0,
        };
        v.reload();
        v.show(page, false, cx);
        v
    }

    /// Put `page` on screen (`embedded`: inside the setup wizard). Other
    /// pages hide this view.
    pub fn show(&mut self, page: Page, embedded: bool, cx: &mut Context<Self>) {
        self.visible = matches!(page, Page::Device | Page::Accounts | Page::Hooks | Page::Settings);
        if !self.visible {
            return;
        }
        self.page = page;
        self.embedded = embedded;
        self.reload();
        if page == Page::Device && self.usb && !self.device.probing && self.device.port.is_none() {
            self.probe_device(cx);
        }
        cx.notify();
    }

    /// Accounts whose hooks need installing or updating.
    pub fn pending_hooks(&self) -> usize {
        self.hooks.iter().filter(|h| h.status.needs_install()).count()
    }

    /// Open the review of installing hooks in every account that needs them
    /// (what the page's install button does).
    pub fn review_hook_install(&mut self, cx: &mut Context<Self>) {
        let pending = self.hooks.iter().filter(|h| h.status.needs_install()).map(|h| h.dir.clone()).collect();
        self.plan_hooks(HookAction::Install, pending);
        cx.notify();
    }

    /// Open or close the Settings page's Advanced section.
    pub fn show_advanced(&mut self, open: bool, cx: &mut Context<Self>) {
        self.advanced = open;
        cx.notify();
    }

    /// Accounts whose settings.json couldn't be read.
    pub fn unreadable_hooks(&self) -> usize {
        self.hooks.iter().filter(|h| matches!(h.status, HookStatus::Unreadable(_))).count()
    }

    /// Accounts the hooks page lists (every configured account).
    pub fn hook_accounts(&self) -> usize {
        self.hooks.len()
    }

    /// Accounts whose hooks are set up.
    pub fn installed_hooks(&self) -> usize {
        self.hooks.iter().filter(|h| h.status == HookStatus::Installed).count()
    }

    fn reload(&mut self) {
        match Settings::load(&self.paths) {
            Ok(s) => {
                self.settings = s;
                self.config_error = None;
            }
            Err(e) => {
                self.config_error = Some(format!(
                    "Couldn't read {}: {e}",
                    tilde(&self.paths.config_file.display().to_string(), &self.paths.home)
                ))
            }
        }
        self.hooks = self
            .settings
            .config_dirs
            .iter()
            .map(|d| HookRow {
                dir: d.clone(),
                status: match &self.hook_exe {
                    Some(exe) => hooks::dir_status(d, exe),
                    None => HookStatus::Unreadable("Couldn't locate the Clawdmeter executable.".into()),
                },
            })
            .collect();
        self.autostart_on = autostart::is_enabled(&self.paths.home, LaunchMode::Tray);
        self.autostart_stale = self
            .launch_exe
            .as_ref()
            .filter(|want| autostart::is_stale(&self.paths.home, LaunchMode::Tray, want))
            .map(|_| autostart::entry_exe(&self.paths.home, LaunchMode::Tray));
        self.legacy = autostart::legacy_entry(&self.paths.home);
        self.snap = live_status(&self.paths);
    }

    fn tick(&mut self, cx: &mut Context<Self>) {
        self.ticks += 1;
        self.snap = live_status(&self.paths);
        // Pick up hand edits and other windows' changes every few seconds,
        // but never under an open diff preview.
        if self.ticks.is_multiple_of(6) && self.preview.is_none() {
            self.reload();
        }
        // Notice a board being plugged in (only while the Device page is up,
        // so it never competes with the flasher for the port).
        let on_device = self.visible && self.page == Page::Device;
        if self.usb && on_device && self.ticks.is_multiple_of(4) && !self.device.probing && self.device.port.is_none() {
            if let Some(p) = device::list_ports().into_iter().next() {
                self.device.port = Some(p.path);
                self.probe_device(cx);
            }
        }
        cx.notify();
    }

    /// Change settings: re-read the file, apply the change, write only the
    /// keys whose value differs.
    fn save(&mut self, change: impl FnOnce(&mut Settings, &mut ConfigFile)) {
        let path = self.paths.config_file.clone();
        match write_settings(&self.paths, change) {
            Ok(()) => self.notice = None,
            Err(e) => self.notice = Some((Tone::Bad, errors::save_failed("settings", &path, &self.paths.home, &e))),
        }
        self.reload();
    }

    fn probe_device(&mut self, cx: &mut Context<Self>) {
        self.device.probing = true;
        run_blocking(
            cx,
            || {
                let port = device::list_ports().into_iter().next().map(|p| p.path);
                let Some(port) = port else { return (None, None) };
                let info = device::probe_info(&port, Duration::from_millis(1500)).ok().flatten();
                (Some(port), info)
            },
            |v, (port, info), _| {
                v.device.probing = false;
                v.device.port = port;
                if let Some(i) = &info {
                    let _ =
                        ipc::save_firmware_record(&v.paths, &FirmwareRecord { env: i.env.clone(), ver: i.ver.clone() });
                }
                v.device.info = info;
            },
        );
    }

    /// Clear every Bluetooth bond on the board (firmware `unpair`), so it
    /// can pair with this or another computer again.
    fn unpair(&mut self, cx: &mut Context<Self>) {
        let Some(port) = self.device.port.clone() else { return };
        self.device.busy = true;
        self.device.unpair = None;
        run_blocking(
            cx,
            move || device::query_json(&port, "unpair", "unpair", Duration::from_millis(1500)),
            |v, r, _| {
                v.device.busy = false;
                v.device.unpair = Some(match r {
                    Ok(Some(_)) => (
                        Tone::Ok,
                        "The board's pairing is cleared. Remove Clawdmeter from this computer's Bluetooth settings, \
                         then pair it again."
                            .into(),
                    ),
                    Ok(None) => (
                        Tone::Bad,
                        "No response from the device. Reconnect USB or update the firmware, then try again.".into(),
                    ),
                    Err(e) => (Tone::Bad, e.to_string()),
                });
            },
        );
    }

    fn add_account(&mut self, cx: &mut Context<Self>) {
        let rx = cx.prompt_for_paths(PathPromptOptions {
            files: false,
            directories: true,
            multiple: false,
            prompt: Some("Add Claude Code account folder".into()),
        });
        cx.spawn(async move |this, cx| {
            if let Ok(Ok(Some(paths))) = rx.await {
                let _ = this.update(cx, |v, cx| {
                    for p in paths {
                        if !v.settings.config_dirs.contains(&p) {
                            v.save(|s, _| s.config_dirs.push(p));
                        }
                    }
                    cx.notify();
                });
            }
        })
        .detach();
    }

    /// The binary to set the hooks (`hooks`) or the login entry up with,
    /// or why not.
    fn launchable(&self, hooks: bool) -> Result<PathBuf, String> {
        let missing = || "Couldn't locate the Clawdmeter executable.".to_owned();
        let exe = self.launch_exe.clone().ok_or_else(missing)?;
        if let Some(u) = clawdmeter_core::launch::unstable(&exe) {
            return Err(format!("Clawdmeter is running from {}. {}", u.location_noun(), u.advice()));
        }
        if hooks {
            self.hook_exe.clone().ok_or_else(missing)
        } else {
            Ok(exe)
        }
    }

    fn plan_hooks(&mut self, action: HookAction, dirs: Vec<PathBuf>) {
        self.hook_msg = None;
        let exe = match (action, self.launchable(true)) {
            (_, Ok(exe)) => exe,
            (HookAction::Install, Err(why)) => {
                self.hook_msg = Some((Tone::Bad, why));
                return;
            }
            // Removing doesn't write a path.
            (HookAction::Remove, Err(_)) => match self.hook_exe.clone() {
                Some(exe) => exe,
                None => return,
            },
        };
        let mut plans = Vec::new();
        for d in dirs {
            let plan = match action {
                HookAction::Install => hooks::plan_install(&d, &exe),
                HookAction::Remove => hooks::plan_uninstall(&d),
            };
            match plan {
                Ok(p) if p.changes() => plans.push(p),
                Ok(_) => {}
                Err(e) => {
                    let f = tilde(&d.join("settings.json").display().to_string(), &self.paths.home);
                    self.hook_msg = Some((Tone::Bad, format!("{f}: {e}")));
                    return;
                }
            }
        }
        if plans.is_empty() {
            self.hook_msg = Some((Tone::Ok, "No changes needed.".into()));
        } else {
            self.preview = Some(HookPreview { action, plans });
        }
    }

    fn apply_hooks(&mut self) {
        let Some(p) = self.preview.take() else { return };
        let mut done = 0;
        for plan in &p.plans {
            if let Err(e) = plan.apply() {
                // Files are written one by one: say which ones made it.
                let what = tilde(&plan.path.display().to_string(), &self.paths.home);
                let saved = match done {
                    0 => String::new(),
                    1 => " 1 other file was already saved.".into(),
                    n => format!(" {n} other files were already saved."),
                };
                self.hook_msg = Some((Tone::Bad, format!("Couldn't write {what}. {e}{saved}")));
                self.reload();
                return;
            }
            done += 1;
        }
        let accounts = if done == 1 { "1 account".to_string() } else { format!("{done} accounts") };
        self.hook_msg = Some((
            Tone::Ok,
            match p.action {
                HookAction::Install => {
                    format!("Hooks installed in {accounts}. Your original settings are kept in settings.json.bak.")
                }
                HookAction::Remove => {
                    format!("Hooks removed from {accounts}. Your original settings are kept in settings.json.bak.")
                }
            },
        ));
        self.reload();
    }

    fn send_test_alert(&mut self, cx: &mut Context<Self>) {
        let path = self.paths.attention_file();
        let body = serde_json::json!({"m": "Clawdmeter test alert", "s": clawdmeter_core::attention::TEST_SESSION});
        let written =
            std::fs::create_dir_all(&self.paths.state_dir).and_then(|_| std::fs::write(&path, body.to_string()));
        self.test_alert = Some(match written {
            Ok(()) if self.snap.is_none() => Err("Clawdmeter isn't running.".into()),
            Ok(()) => Ok(()),
            Err(e) => Err(format!("Couldn't send it. {}", errors::io_message(&e))),
        });
        self.test_alert_seq += 1;
        let seq = self.test_alert_seq;
        let hold = if self.test_alert == Some(Ok(())) { TEST_ALERT_SENT } else { TEST_ALERT_FAILED };
        cx.spawn(async move |this, cx| {
            cx.background_executor().timer(hold).await;
            let _ = this.update(cx, |v, cx| {
                if v.test_alert_seq == seq {
                    v.test_alert = None;
                    cx.notify();
                }
            });
        })
        .detach();
    }

    // ---------------------------------------------------------------- panes

    fn device_pane(&mut self, t: &Theme, cx: &mut Context<Self>) -> gpui::Div {
        let snap = self.snap.clone();
        let rec = ipc::load_firmware_record(&self.paths);
        let env = self.device.info.as_ref().map(|i| i.env.clone()).or_else(|| rec.as_ref().map(|r| r.env.clone()));
        let ver = self
            .device
            .info
            .as_ref()
            .map(|i| i.ver.clone())
            .or_else(|| rec.as_ref().map(|r| r.ver.clone()))
            .or_else(|| snap.as_ref().and_then(|s| s.device_firmware.clone()));
        let board = env.as_deref().and_then(boards::board);
        let name: SharedString = match (board, &env) {
            (Some(b), _) => b.name.into(),
            (None, Some(e)) => e.clone().into(),
            (None, None) => "Board not identified".into(),
        };
        // Without a live USB answer, the identity is what was last seen.
        let cached = self.device.info.is_none() && env.is_some();
        let detail: Option<SharedString> = match (board, cached) {
            (Some(b), true) => Some(format!("{} · last detected", b.product).into()),
            (Some(b), false) => Some(b.product.into()),
            (None, true) => Some("Last detected".into()),
            (None, false) => None,
        };
        let update = snap.as_ref().and_then(|s| s.update_available().map(str::to_owned));
        let (link_tone, link): (Tone, SharedString) = match snap.as_ref() {
            None => (Tone::Plain, "Clawdmeter isn't running".into()),
            Some(s) => match s.link.as_str() {
                "connected" => (Tone::Ok, "Bluetooth connected".into()),
                "no_adapter" => (Tone::Bad, "Bluetooth unavailable".into()),
                "connecting" => (Tone::Accent, "Connecting…".into()),
                _ => (Tone::Plain, "Looking for Clawdmeter".into()),
            },
        };
        let usb: SharedString = match (&self.device.port, self.device.probing) {
            (_, true) => "Checking USB…".into(),
            (Some(p), _) => format!("USB: {p}").into(),
            (None, _) if board.is_none() => "Connect USB to identify the board.".into(),
            (None, _) => "USB not connected".into(),
        };
        let firmware: SharedString = match (&ver, &update) {
            (Some(v), Some(u)) => {
                format!("Firmware v{} · v{} available", v.trim_start_matches('v'), u.trim_start_matches('v')).into()
            }
            (Some(v), None) => format!("Firmware v{}", v.trim_start_matches('v')).into(),
            (None, _) => "Firmware version unknown".into(),
        };
        let art = div().flex().flex_shrink_0().items_center().justify_center().size(px(150.)).child(match board {
            Some(b) => img(b.illustration()).size(px(150.)).into_any_element(),
            None => super::widgets::clawd(4.).into_any_element(),
        });
        let (label, kind) = match &update {
            Some(_) => ("Update firmware", ButtonKind::Primary),
            None => ("Flash firmware…", ButtonKind::Normal),
        };
        let card = card(t).flex_row().items_center().gap(px(20.)).p(px(20.)).child(art).child(
            div()
                .flex()
                .flex_col()
                .flex_1()
                .min_w_0()
                .gap(px(8.))
                .child(
                    div()
                        .flex()
                        .flex_col()
                        .gap(px(2.))
                        .child(div().font_family(FONT_DISPLAY).text_size(px(20.)).child(name))
                        .when_some(detail, |d, x| d.child(div().text_size(px(12.5)).text_color(t.mute).child(x))),
                )
                .child(div().flex().flex_wrap().gap(px(6.)).child(pill(t, link, link_tone)))
                .child(div().text_size(px(13.)).text_color(t.dim).child(firmware))
                .child(div().text_size(px(13.)).text_color(t.mute).child(usb))
                .child(div().flex().pt(px(4.)).child(button(
                    t,
                    "update-now",
                    label,
                    kind,
                    true,
                    cx.listener(|_, _, _, cx| cx.emit(Navigate(Page::Flash))),
                ))),
        );
        let mut pane = div()
            .flex()
            .flex_col()
            .gap(px(6.))
            .child(heading(t, "Device"))
            .child(div().pt(px(8.)).pb(px(10.)).child(card));

        let me = cx.entity().downgrade();
        pane = pane.child(field(
            t,
            "Clock",
            "Replaces the Usage title with a clock. Auto follows this computer's time format.",
            segmented(
                t,
                "clock",
                &[
                    (ClockMode::Off, "Off"),
                    (ClockMode::Auto, "Auto"),
                    (ClockMode::H12, "12 h"),
                    (ClockMode::H24, "24 h"),
                ],
                &self.settings.clock,
                true,
                move |m, _, cx| {
                    let _ = me.update(cx, |v, cx| {
                        v.save(|s, _| s.clock = m);
                        cx.notify();
                    });
                },
            ),
        ));
        let (me1, me2) = (cx.entity().downgrade(), cx.entity().downgrade());
        let (alert, reset) = (self.settings.alert_chime, self.settings.reset_chime);
        pane = pane
            .child(field(
                t,
                "Alert chime",
                "Chimes when Claude needs you, on boards with a speaker. Mute from the tray until restart.",
                toggle(t, "alert-chime", alert, true, move |_, _, cx| {
                    let _ = me1.update(cx, |v, cx| {
                        v.save(|s, _| s.alert_chime = !alert);
                        cx.notify();
                    });
                }),
            ))
            .child(field(
                t,
                "Session reset chime",
                "Chimes when your session usage resets, on boards with a speaker.",
                toggle(t, "reset-chime", reset, true, move |_, _, cx| {
                    let _ = me2.update(cx, |v, cx| {
                        v.save(|s, _| s.reset_chime = !reset);
                        cx.notify();
                    });
                }),
            ));
        let me = cx.entity().downgrade();
        let on = self.settings.update_check;
        pane = pane.child(field(
            t,
            "Check for firmware updates",
            "Checks GitHub daily. Updates appear here and in the tray.",
            toggle(t, "update-check", on, true, move |_, _, cx| {
                let _ = me.update(cx, |v, cx| {
                    v.save(|s, _| s.update_check = !on);
                    cx.notify();
                });
            }),
        ));
        let p = self.paths.clone();
        pane = pane.child(field(
            t,
            "Clear saved device",
            "Clears the app's saved address and searches for a paired Clawdmeter. Bluetooth pairing stays in place.",
            button(t, "forget", "Clear", ButtonKind::Small, true, move |_, _, _| command(&p, AppCommand::Forget)),
        ));
        let on_usb = self.device.port.is_some();
        let me = cx.entity().downgrade();
        pane = pane.child(field(
            t,
            "Clear the board's pairing",
            if on_usb {
                "Makes the board forget every computer it was paired with, so it can pair again."
            } else {
                "Makes the board forget every computer it was paired with. Connect USB to use this."
            },
            button(t, "unpair", "Clear pairing", ButtonKind::Small, on_usb && !self.device.busy, move |_, _, cx| {
                let _ = me.update(cx, |v, cx| v.unpair(cx));
            }),
        ));
        if let Some((tone, msg)) = &self.device.unpair {
            pane = pane.child(notice(t, *tone, msg.clone()));
        }
        pane
    }

    fn accounts_pane(&mut self, t: &Theme, cx: &mut Context<Self>) -> gpui::Div {
        let me = cx.entity().downgrade();
        let mut pane = div()
            .flex()
            .flex_col()
            .gap(px(14.))
            .child(div().flex().items_center().justify_between().child(heading(t, "Accounts")).child(button(
                t,
                "add-dir",
                "Add account folder…",
                ButtonKind::Small,
                true,
                move |_, _, cx| {
                    let _ = me.update(cx, |v, cx| v.add_account(cx));
                },
            )))
            .child(para(
                t,
                "The default Claude Code folder is ~/.claude. Add a config folder for each other account you want \
                 to monitor. The device follows the account whose session usage increased most recently.",
            ))
            .when_some(self.snap.as_ref(), |d, snap| {
                d.child(caption(
                    t,
                    match &snap.claude {
                        Some(c) => {
                            format!("Usage is read with {}.", crate::claude_label(Path::new(c), &self.paths.home))
                        }
                        None => "Claude Code not found, usage updates every 12 min.".to_owned(),
                    },
                ))
            });
        let active = self.snap.as_ref().and_then(|s| s.account.clone());
        let errors: Vec<ipc::AccountError> = self.snap.as_ref().map(|s| s.errors.clone()).unwrap_or_default();
        let only_one = self.settings.config_dirs.len() <= 1;
        let mut list = div().flex().flex_col().rounded(px(R_M)).border_1().border_color(t.line);
        for (i, d) in self.settings.config_dirs.clone().into_iter().enumerate() {
            let ds = d.display().to_string();
            let err = errors.iter().find(|e| e.dir == ds).cloned();
            let is_active = active.as_deref() == Some(ds.as_str());
            let (meta, chip) = match (&err, is_active) {
                // Transient errors retry on their own; the rest need the user.
                (Some(e), _) if e.retrying => (e.message.clone(), Some(pill(t, "Retrying", Tone::Warn))),
                (Some(e), _) => (e.message.clone(), Some(pill(t, "Needs attention", Tone::Bad))),
                (None, true) => (String::new(), Some(pill(t, "On device", Tone::Ok))),
                (None, false) if self.snap.is_some() => ("Not selected for the device".to_string(), None),
                (None, false) => ("Start Clawdmeter to check usage.".to_string(), None),
            };
            let me = cx.entity().downgrade();
            let dir = d.clone();
            list = list.child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(12.))
                    .px(px(14.))
                    .py(px(12.))
                    .when(i > 0, |r| r.border_t_1().border_color(t.line))
                    .child(
                        div()
                            .flex()
                            .flex_col()
                            .flex_1()
                            .min_w_0()
                            .gap(px(2.))
                            .child(div().font_family(FONT_MONO).text_size(px(13.)).child(tilde(&ds, &self.paths.home)))
                            .when(!meta.is_empty(), |d| {
                                d.child(div().text_size(px(12.5)).text_color(t.mute).child(meta))
                            }),
                    )
                    .when_some(chip, |r, c| r.child(c))
                    .child(button(
                        t,
                        SharedString::from(format!("rm-{i}")),
                        "Remove",
                        ButtonKind::Small,
                        !only_one,
                        move |_, _, cx| {
                            let dir = dir.clone();
                            let _ = me.update(cx, |v, cx| {
                                v.save(|s, _| s.config_dirs.retain(|x| *x != dir));
                                cx.notify();
                            });
                        },
                    )),
            );
        }
        pane = pane.child(list).child(caption(
            t,
            if only_one {
                "Removing a folder stops monitoring it; its files and installed hooks stay in place. The last \
                 folder can't be removed."
            } else {
                "Removing a folder stops monitoring it; its files and installed hooks stay in place."
            },
        ));
        if self.snap.is_none() {
            pane = pane.child(notice(t, Tone::Plain, not_running_text()));
        }
        pane
    }

    fn hooks_pane(&mut self, t: &Theme, cx: &mut Context<Self>) -> gpui::Div {
        let mut pane = div().flex().flex_col().gap(px(14.)).when(!self.embedded, |d| {
            d.child(heading(t, "Hooks")).child(para(
                t,
                "Hooks let Clawd react to activity and show alerts. The event log stores timestamps, event types, \
                 and session IDs. Alerts also store notification or question text for the device. Usage meters work \
                 without hooks.",
            ))
        });
        let busy = self.preview.is_some();
        // With one account to change, the review button below is the only
        // entry point; rows keep their own actions when there are several.
        let single = self.pending_hooks() == 1;
        let mut list = div().flex().flex_col().rounded(px(R_M)).border_1().border_color(t.line);
        for (i, row) in self.hooks.iter().enumerate() {
            let (meta, chip, action): (String, (&str, Tone), Option<(&str, HookAction)>) = match &row.status {
                HookStatus::Installed => (
                    format!("{} hooks installed", HOOKS.len()),
                    ("Installed", Tone::Ok),
                    Some(("Review removal", HookAction::Remove)),
                ),
                HookStatus::Missing => (
                    "Clawdmeter hooks are not installed. Other hooks are kept.".into(),
                    ("Not installed", Tone::Plain),
                    Some(("Review install", HookAction::Install)),
                ),
                HookStatus::Legacy => (
                    "Uses attention-hook.sh. Update to the built-in Clawdmeter hooks.".into(),
                    ("Legacy hooks", Tone::Warn),
                    Some(("Review update", HookAction::Install)),
                ),
                HookStatus::Stale(old) => (
                    if let Some(u) = clawdmeter_core::launch::unstable(Path::new(old)) {
                        format!("Runs {}, {}.", tilde(old, &self.paths.home), u.location())
                    } else if Path::new(old).exists() {
                        format!("Uses another Clawdmeter executable: {}.", tilde(old, &self.paths.home))
                    } else {
                        format!("Runs {}, which no longer exists.", tilde(old, &self.paths.home))
                    },
                    ("Out of date", Tone::Warn),
                    Some(("Review update", HookAction::Install)),
                ),
                HookStatus::Partial => (
                    "Some hooks are missing.".into(),
                    ("Incomplete", Tone::Warn),
                    Some(("Review update", HookAction::Install)),
                ),
                HookStatus::Unreadable(e) => (e.clone(), ("Unreadable", Tone::Bad), None),
            };
            let mut r = div()
                .flex()
                .items_center()
                .gap(px(12.))
                .px(px(14.))
                .py(px(12.))
                .when(i > 0, |r| r.border_t_1().border_color(t.line))
                .child(
                    div()
                        .flex()
                        .flex_col()
                        .flex_1()
                        .min_w_0()
                        .gap(px(3.))
                        .child(
                            div()
                                .font_family(FONT_MONO)
                                .text_size(px(13.))
                                .truncate()
                                .child(tilde(&row.dir.join("settings.json").display().to_string(), &self.paths.home)),
                        )
                        .child(div().text_size(px(12.5)).text_color(t.mute).child(meta)),
                )
                .child(pill(t, chip.0, chip.1));
            let action = action.filter(|(_, a)| !(single && *a == HookAction::Install));
            if let Some((label, action)) = action {
                let me = cx.entity().downgrade();
                let dir = row.dir.clone();
                r = r.child(button(
                    t,
                    SharedString::from(format!("hook-{i}")),
                    label,
                    ButtonKind::Small,
                    !busy,
                    move |_, _, cx| {
                        let dir = dir.clone();
                        let _ = me.update(cx, |v, cx| {
                            v.plan_hooks(action, vec![dir]);
                            cx.notify();
                        });
                    },
                ));
            }
            list = list.child(r);
        }
        pane = pane.child(list);

        if let Some(p) = &self.preview {
            let n = p.plans.len();
            let title = match (p.action, n) {
                (HookAction::Install, 1) => "Review 1 file".to_string(),
                (HookAction::Install, n) => format!("Review {n} files"),
                (HookAction::Remove, _) => "Review hook removal".to_string(),
            };
            let (me1, me2) = (cx.entity().downgrade(), cx.entity().downgrade());
            let apply = match p.action {
                HookAction::Install => "Install hooks",
                HookAction::Remove => "Remove hooks",
            };
            pane = pane.child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(8.))
                    .pt(px(6.))
                    .child(div().flex_1().child(subheading(t, title)))
                    .child(button(t, "apply-hooks", apply, ButtonKind::Primary, true, move |_, _, cx| {
                        let _ = me1.update(cx, |v, cx| {
                            v.apply_hooks();
                            cx.notify();
                        });
                    }))
                    .child(button(t, "cancel-hooks", "Cancel", ButtonKind::Normal, true, move |_, _, cx| {
                        let _ = me2.update(cx, |v, cx| {
                            v.preview = None;
                            cx.notify();
                        });
                    })),
            );
            pane =
                pane.child(caption(t, "Review before applying. Your original settings are kept in settings.json.bak."));
            for plan in &p.plans {
                // Diff what changes, not how the file was formatted: compare
                // against the old file pretty-printed the way it will be saved.
                let before = plan.before.as_deref().unwrap_or("");
                let pretty_before = serde_json::from_str::<serde_json::Value>(before)
                    .ok()
                    .and_then(|v| serde_json::to_string_pretty(&v).ok())
                    .map(|s| s + "\n");
                let reformatted = pretty_before.as_deref().is_some_and(|p| p.trim() != before.trim());
                let diff = with_context(line_diff(pretty_before.as_deref().unwrap_or(before), &plan.after), 2);
                let mut block = div()
                    .flex()
                    .flex_col()
                    .py(px(10.))
                    .rounded(px(R_S))
                    .bg(t.bg)
                    .border_1()
                    .border_color(t.line)
                    .font_family(FONT_MONO)
                    .text_size(px(12.))
                    .line_height(px(18.));
                for l in diff {
                    let (sign, text, fg, bg) = match l {
                        DiffLine::Same(s) => (" ", s, t.mute, None),
                        DiffLine::Added(s) => ("+", s, t.ok, Some(with_alpha(t.ok, 0.10))),
                        DiffLine::Removed(s) => ("-", s, t.bad, Some(with_alpha(t.bad, 0.10))),
                    };
                    block = block.child(
                        div()
                            .flex()
                            .px(px(12.))
                            .text_color(fg)
                            .when_some(bg, |d, bg| d.bg(bg))
                            .child(div().w(px(16.)).flex_shrink_0().child(sign))
                            .child(div().flex_1().min_w_0().child(text)),
                    );
                }
                let file = tilde(&plan.path.display().to_string(), &self.paths.home);
                let what = match (plan.before.is_none(), reformatted) {
                    (true, _) => format!("{file} (new file)"),
                    (false, true) => format!("{file} (also re-indented)"),
                    (false, false) => file,
                };
                pane = pane.child(div().flex().flex_col().gap(px(6.)).child(caption(t, what)).child(block));
            }
        } else {
            let pending: Vec<PathBuf> =
                self.hooks.iter().filter(|h| h.status.needs_install()).map(|h| h.dir.clone()).collect();
            let any_old = self.hooks.iter().any(|h| h.status.needs_install() && h.status != HookStatus::Missing);
            let me1 = cx.entity().downgrade();
            let n = pending.len();
            let label = match (any_old, n) {
                (true, 1) => "Review update".to_string(),
                (true, n) => format!("Review updates for {n} accounts"),
                (false, 1) => "Review installation".to_string(),
                (false, n) => format!("Review installation in {n} accounts"),
            };
            pane = pane.child(div().flex().items_center().gap(px(8.)).when(n > 0, |d| {
                d.child(button(t, "install-all", label, ButtonKind::Primary, true, move |_, _, cx| {
                    let pending = pending.clone();
                    let _ = me1.update(cx, |v, cx| {
                        v.plan_hooks(HookAction::Install, pending);
                        cx.notify();
                    });
                }))
            }));
        }
        if let Some((tone, m)) = &self.hook_msg {
            pane = pane.child(notice(t, *tone, m.clone()));
        }
        if !self.embedded && self.preview.is_none() {
            pane = pane.child(self.alert_kinds(t, cx));
        }
        pane
    }

    /// Which waits alert (`alert_on`), for every account at once. The hooks
    /// stay installed as they are; the app drops the kinds that are off.
    fn alert_kinds(&self, t: &Theme, cx: &mut Context<Self>) -> impl IntoElement {
        type Pick = fn(&mut AlertOn) -> &mut bool;
        let rows: [(&'static str, &'static str, &'static str, Pick); 4] = [
            ("alert-permission", "Tool approvals", "Claude needs permission to run a tool.", |a| &mut a.permission),
            ("alert-question", "Questions", "Claude has a question for you.", |a| &mut a.question),
            ("alert-idle", "Idle sessions", "Claude has been waiting for your next prompt.", |a| &mut a.idle),
            ("alert-elicitation", "MCP requests", "An MCP server needs information from you.", |a| &mut a.elicitation),
        ];
        let mut list = div()
            .flex()
            .flex_col()
            .pt(px(10.))
            .child(subheading(t, "Alert types"))
            .child(caption(t, "Applies to all accounts, on the device and in the tray."));
        let current = self.settings.alert_on;
        for (id, label, help, pick) in rows {
            let mut probe = current;
            let on = *pick(&mut probe);
            let me = cx.entity().downgrade();
            list = list.child(checkbox(t, id, label, help, on, move |_, _, cx| {
                let _ = me.update(cx, |v, cx| {
                    v.save(|s, _| {
                        let flag = pick(&mut s.alert_on);
                        *flag = !*flag;
                    });
                    cx.notify();
                });
            }));
        }
        // Try the alert types (and the chimes) from here.
        let me = cx.entity().downgrade();
        let label = if self.test_alert == Some(Ok(())) { "Sent" } else { "Send test alert" };
        list.child(
            div()
                .flex()
                .items_center()
                .gap(px(8.))
                .pt(px(12.))
                .child(button(t, "test-alert", label, ButtonKind::Normal, true, move |_, _, cx| {
                    let _ = me.update(cx, |v, cx| {
                        v.send_test_alert(cx);
                        cx.notify();
                    });
                }))
                .when_some(self.test_alert.clone().and_then(Result::err), |d, why| {
                    d.child(div().text_size(px(12.5)).text_color(t.bad).child(why))
                }),
        )
    }

    fn settings_pane(&mut self, t: &Theme, cx: &mut Context<Self>) -> gpui::Div {
        let me = cx.entity().downgrade();
        let on = self.autostart_on;
        let home = self.paths.home.clone();
        let mut pane = div().flex().flex_col().gap(px(6.)).child(heading(t, "Settings")).child(field(
            t,
            "Start at login",
            "Opens Clawdmeter when you sign in.",
            toggle(t, "autostart", on, true, move |_, _, cx| {
                let home = home.clone();
                let _ = me.update(cx, |v, cx| {
                    let r = match v.launchable(false) {
                        _ if on => autostart::disable(&home, LaunchMode::Tray).map_err(|e| errors::io_message(&e)),
                        Ok(exe) => autostart::enable(&home, &exe, LaunchMode::Tray).map_err(|e| errors::io_message(&e)),
                        Err(why) => Err(why),
                    };
                    if let Err(why) = r {
                        v.notice = Some((Tone::Bad, format!("Couldn't change Start at login. {why}")));
                    }
                    v.reload();
                    cx.notify();
                });
            }),
        ));
        if let (Some(old), Some(exe)) = (self.autostart_stale.clone(), self.launch_exe.clone()) {
            let text = match old {
                Some(o) if clawdmeter_core::launch::unstable(&o).is_some() => format!(
                    "Start at login opens {}, {}.",
                    tilde(&o.display().to_string(), &self.paths.home),
                    clawdmeter_core::launch::unstable(&o).map_or("", |u| u.location())
                ),
                Some(o) if !o.exists() => format!(
                    "Start at login opens {}, which no longer exists.",
                    tilde(&o.display().to_string(), &self.paths.home)
                ),
                Some(o) => format!(
                    "Start at login opens another copy of Clawdmeter: {}.",
                    tilde(&o.display().to_string(), &self.paths.home)
                ),
                None => "Start at login can't be read.".into(),
            };
            let me = cx.entity().downgrade();
            let home = self.paths.home.clone();
            pane =
                pane.child(div().pb(px(6.)).child(notice(t, Tone::Warn, text).child(div().flex().pl(px(18.)).child(
                    button(t, "fix-autostart", "Use this copy", ButtonKind::Small, true, move |_, _, cx| {
                        let _ = me.update(cx, |v, cx| {
                            let r = match v.launchable(false) {
                                Ok(_) => {
                                    autostart::enable(&home, &exe, LaunchMode::Tray).map_err(|e| errors::io_message(&e))
                                }
                                Err(why) => Err(why),
                            };
                            if let Err(why) = r {
                                v.notice = Some((Tone::Bad, format!("Couldn't change Start at login. {why}")));
                            }
                            v.reload();
                            cx.notify();
                        });
                    }),
                ))));
        }

        let poll = if self.settings.poll_interval_explicit {
            PollChoice::Every(self.settings.poll_interval)
        } else {
            PollChoice::Auto
        };
        let mut options = vec![
            (PollChoice::Auto, "Automatic"),
            (PollChoice::Every(120), "While working: 2 min"),
            (PollChoice::Every(300), "While working: 5 min"),
        ];
        let custom;
        if let PollChoice::Every(n) = poll {
            if n != 120 && n != 300 {
                custom = format!("While working: {n} s");
                options.push((poll, custom.as_str()));
            }
        }
        let me = cx.entity().downgrade();
        pane = pane.child(field_stacked(
            t,
            "Usage checks",
            "Automatic checks every 3 minutes while working, then less often when quiet. Retry delays apply to \
             every check.",
            segmented(t, "poll", &options, &poll, true, move |c, _, cx| {
                let _ = me.update(cx, |v, cx| {
                    let paths = v.paths.clone();
                    v.save(|s, cfg| match c {
                        PollChoice::Auto => {
                            cfg.remove("poll_interval");
                            let fresh = Settings::from_config(cfg, &paths, None);
                            s.poll_interval = fresh.poll_interval;
                            s.poll_interval_explicit = false;
                        }
                        PollChoice::Every(n) => {
                            s.poll_interval = n;
                            cfg.set("poll_interval", &n.to_string());
                        }
                    });
                    cx.notify();
                });
            }),
        ));
        // A missing CLI stops everything, so it isn't tucked under Advanced.
        if self.snap.as_ref().is_some_and(|s| s.claude.is_none()) {
            let cfgp = self.paths.config_file.clone();
            let label = if cfgp.exists() { "Open config" } else { "Create config" };
            pane = pane.child(
                div().pt(px(12.)).child(
                    notice(
                        t,
                        Tone::Plain,
                        "Claude Code wasn't found, so usage updates every 12 minutes. Install Claude Code or the Claude \
                         desktop app for updates while you work, or set claude_path in the config file.",
                    )
                    .child(div().flex().pl(px(18.)).child(button(
                        t,
                        "fix-cli",
                        label,
                        ButtonKind::Small,
                        true,
                        move |_, _, cx| open_config(&cfgp, cx),
                    ))),
                ),
            );
        }
        let p = self.paths.clone();
        pane = pane.child(field(
            t,
            "Quit Clawdmeter",
            "Stops usage updates and closes Clawdmeter. The device retains its last reading.",
            button(t, "quit", "Quit", ButtonKind::Small, self.snap.is_some(), move |_, window, _| {
                command(&p, AppCommand::Quit);
                window.remove_window();
            }),
        ));
        if let Some(legacy) = self.legacy.clone() {
            let me = cx.entity().downgrade();
            let home = self.paths.home.clone();
            pane = pane.child(field(
                t,
                "Legacy service detected",
                format!(
                    "Disable {} so only one Clawdmeter process connects to the device.",
                    tilde(&legacy.describe(), &self.paths.home)
                ),
                button(t, "legacy", "Disable legacy service", ButtonKind::Primary, true, move |_, _, cx| {
                    autostart::disable_legacy(&home);
                    let _ = me.update(cx, |v, cx| {
                        v.reload();
                        if let Some(still) = &v.legacy {
                            v.notice = Some((
                                Tone::Bad,
                                format!(
                                    "Couldn't disable {}. Remove it by hand.",
                                    tilde(&still.describe(), &v.paths.home)
                                ),
                            ));
                        }
                        cx.notify();
                    });
                }),
            ));
        }
        // Rarely needed: where things live and which CLI runs.
        let open = self.advanced;
        let text = t.text;
        pane = pane.child(
            div()
                .id("advanced")
                .group("advanced")
                .debug_selector(|| "advanced".into())
                .flex()
                .items_center()
                .gap(px(6.))
                .pt(px(14.))
                .pb(px(4.))
                .text_size(px(14.))
                .text_color(t.dim)
                .cursor_pointer()
                .hover(|s| s.text_color(t.text))
                .on_click(cx.listener(|v, _, _, cx| {
                    v.advanced = !v.advanced;
                    cx.notify();
                }))
                .child(
                    icon(if open { "chevron-down" } else { "chevron-right" }, 16., t.dim)
                        .group_hover("advanced", move |s| s.text_color(text)),
                )
                .child("Advanced"),
        );
        if open {
            let mut advanced = div().flex().flex_col();
            let claude = self.snap.as_ref().and_then(|s| s.claude.clone());
            advanced = advanced.child(field(
                t,
                "Claude Code executable",
                match &claude {
                    Some(c) => format!("Usage is read with {}.", crate::claude_label(Path::new(c), &self.paths.home)),
                    None if self.snap.is_some() => "Not found.".into(),
                    None => "Start Clawdmeter to detect Claude Code.".into(),
                },
                div(),
            ));
            let log = self.paths.state_dir.join("daemon.log");
            advanced = advanced.child(field(
                t,
                "Log file",
                tilde(&log.display().to_string(), &self.paths.home),
                button(t, "open-log", "Open log", ButtonKind::Small, log.exists(), move |_, _, cx| {
                    cx.open_with_system(&log)
                }),
            ));
            let cfgp = self.paths.config_file.clone();
            advanced = advanced.child(field(
                t,
                "Config file",
                tilde(&cfgp.display().to_string(), &self.paths.home),
                button(
                    t,
                    "open-cfg",
                    if cfgp.exists() { "Open config" } else { "Create config" },
                    ButtonKind::Small,
                    true,
                    move |_, _, cx| open_config(&cfgp, cx),
                ),
            ));
            pane = pane.child(advanced);
        }
        pane
    }
}

/// What a new config file starts with: the keys people reach for, commented.
const CONFIG_TEMPLATE: &str = "\
# Clawdmeter settings: key = value. The app re-reads this file within a few seconds.
# claude_path = /path/to/claude
# config_dirs = ~/.claude, ~/.claude-work
# heartbeat_interval = 60
";

/// Open the config file in the system editor, creating it first if needed.
fn open_config(path: &std::path::Path, cx: &mut gpui::App) {
    if !path.exists() {
        let made =
            path.parent().map_or(Ok(()), std::fs::create_dir_all).and_then(|_| std::fs::write(path, CONFIG_TEMPLATE));
        if let Err(e) = made {
            log::warn!("couldn't create {}: {e}", path.display());
            return;
        }
    }
    cx.open_with_system(path);
}

fn with_alpha(c: gpui::Rgba, a: f32) -> gpui::Hsla {
    let mut h: gpui::Hsla = c.into();
    h.a = a;
    h
}

impl Render for SettingsView {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let t = Theme::for_appearance(window.appearance());
        let mut pane = match self.page {
            Page::Accounts => self.accounts_pane(&t, cx),
            Page::Hooks => self.hooks_pane(&t, cx),
            Page::Settings => self.settings_pane(&t, cx),
            _ => self.device_pane(&t, cx),
        };
        if let Some(e) = self.config_error.clone().or_else(|| self.notice.as_ref().map(|(_, m)| m.clone())) {
            pane = pane.child(div().pt(px(12.)).text_size(px(13.)).text_color(t.bad).child(e));
        }
        pane
    }
}

/// Re-read the config file, apply `change`, and write only the keys whose
/// value differs.
pub(crate) fn write_settings(
    paths: &Paths,
    change: impl FnOnce(&mut Settings, &mut ConfigFile),
) -> std::io::Result<()> {
    let path = &paths.config_file;
    let mut cfg = ConfigFile::load(path)?;
    let mut s = Settings::from_config(&cfg, paths, None);
    change(&mut s, &mut cfg);
    s.apply_to(&mut cfg, paths);
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    cfg.save(path)
}

#[cfg(test)]
mod tests {
    use gpui::{Modifiers, TestAppContext, VisualTestContext};

    use super::*;

    fn home(tag: &str) -> Paths {
        let root = std::env::temp_dir().join(format!("cm-settings-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let paths = Paths::under(&root);
        std::fs::create_dir_all(&paths.state_dir).unwrap();
        std::fs::create_dir_all(root.join("acct")).unwrap();
        std::fs::write(&paths.config_file, format!("# mine\nconfig_dirs = {}\n", root.join("acct").display())).unwrap();
        paths
    }

    fn click(cx: &mut VisualTestContext, selector: &'static str) {
        cx.run_until_parked();
        let b = cx.debug_bounds(selector).unwrap_or_else(|| panic!("{selector} isn't on screen"));
        cx.simulate_click(b.center(), Modifiers::none());
        cx.run_until_parked();
    }

    #[gpui::test]
    fn toggles_and_choices_write_the_config(cx: &mut TestAppContext) {
        let paths = home("cfg");
        let p = paths.clone();
        let (view, cx) = cx.add_window_view(move |_, cx| SettingsView::with_usb(p, Page::Device, false, cx));
        click(cx, "reset-chime");
        let text = std::fs::read_to_string(&paths.config_file).unwrap();
        assert!(text.starts_with("# mine\n"), "comments kept: {text}");
        assert!(text.contains("reset_chime = on"), "{text}");

        view.update(cx, |v, cx| v.show(Page::Settings, false, cx));
        click(cx, "poll-2"); // While working: 5 min
        let text = std::fs::read_to_string(&paths.config_file).unwrap();
        assert!(text.contains("poll_interval = 300"), "{text}");
        click(cx, "poll-0"); // Automatic removes the key again.
        assert!(!std::fs::read_to_string(&paths.config_file).unwrap().contains("poll_interval"));
        let _ = std::fs::remove_dir_all(&paths.home);
    }

    #[gpui::test]
    fn hooks_install_after_preview(cx: &mut TestAppContext) {
        let paths = home("hooks");
        let settings = paths.home.join("acct/settings.json");
        let before = r#"{"hooks":{"Stop":[{"hooks":[{"type":"command","command":"/old/attention-hook.sh event"}]}]}}"#;
        std::fs::write(&settings, before).unwrap();
        let p = paths.clone();
        let (view, cx) = cx.add_window_view(move |_, cx| SettingsView::with_usb(p, Page::Hooks, false, cx));
        view.read_with(cx, |v, _| assert_eq!(v.hooks[0].status, HookStatus::Legacy));

        click(cx, "install-all");
        // Nothing is written until the preview is confirmed.
        assert_eq!(std::fs::read_to_string(&settings).unwrap(), before);
        view.read_with(cx, |v, _| assert!(v.preview.is_some()));
        click(cx, "apply-hooks");

        let after = std::fs::read_to_string(&settings).unwrap();
        assert!(!after.contains("attention-hook.sh"), "{after}");
        assert!(after.contains(" hook clear event"), "{after}");
        assert_eq!(std::fs::read_to_string(settings.with_extension("json.bak")).unwrap(), before);
        view.read_with(cx, |v, _| {
            assert_eq!(v.hooks[0].status, HookStatus::Installed);
            assert!(v.preview.is_none());
        });
        let _ = std::fs::remove_dir_all(&paths.home);
    }

    #[cfg(target_os = "linux")]
    #[gpui::test]
    fn stale_login_entry_is_offered_a_fix(cx: &mut TestAppContext) {
        let paths = home("autostart");
        let gone = Path::new("/tmp/.mount_Clawdgone/usr/bin/clawdmeter");
        autostart::enable(&paths.home, gone, LaunchMode::Tray).unwrap();
        let p = paths.clone();
        let (view, cx) = cx.add_window_view(move |_, cx| SettingsView::with_usb(p, Page::Settings, false, cx));
        view.read_with(cx, |v, _| assert_eq!(v.autostart_stale, Some(Some(gone.to_path_buf()))));
        click(cx, "fix-autostart");
        let want = view.read_with(cx, |v, _| {
            assert_eq!(v.autostart_stale, None);
            v.launch_exe.clone().unwrap()
        });
        assert_eq!(autostart::entry_exe(&paths.home, LaunchMode::Tray), Some(want));
        let _ = std::fs::remove_dir_all(&paths.home);
    }

    #[gpui::test]
    fn test_alert_reports_on_its_button(cx: &mut TestAppContext) {
        let paths = home("test-alert");
        let p = paths.clone();
        let (view, cx) = cx.add_window_view(move |_, cx| SettingsView::with_usb(p, Page::Hooks, false, cx));
        click(cx, "test-alert");
        assert!(paths.attention_file().exists());
        // No app-status.json: the app isn't running to pick it up.
        view.read_with(cx, |v, _| {
            assert_eq!(v.test_alert, Some(Err("Clawdmeter isn't running.".into())));
            assert!(v.hook_msg.is_none(), "no banner");
        });
        cx.executor().advance_clock(TEST_ALERT_FAILED);
        cx.run_until_parked();
        view.read_with(cx, |v, _| assert_eq!(v.test_alert, None));
        let _ = std::fs::remove_dir_all(&paths.home);
    }

    #[gpui::test]
    fn alert_kinds_save_to_the_config(cx: &mut TestAppContext) {
        let paths = home("alerts");
        let p = paths.clone();
        let (_view, cx) = cx.add_window_view(move |_, cx| SettingsView::with_usb(p, Page::Hooks, false, cx));
        click(cx, "alert-idle");
        let text = std::fs::read_to_string(&paths.config_file).unwrap();
        assert!(text.contains("alert_on = permission, question, idle, elicitation"), "{text}");
        click(cx, "alert-permission");
        let text = std::fs::read_to_string(&paths.config_file).unwrap();
        assert!(text.contains("alert_on = question, idle, elicitation"), "{text}");
        let _ = std::fs::remove_dir_all(&paths.home);
    }

    #[gpui::test]
    fn unreadable_accounts_are_counted_apart(cx: &mut TestAppContext) {
        let paths = home("unreadable");
        let (a, b) = (paths.home.join("acct"), paths.home.join("broken"));
        std::fs::create_dir_all(&b).unwrap();
        std::fs::write(b.join("settings.json"), "{ not json").unwrap();
        std::fs::write(&paths.config_file, format!("config_dirs = {}, {}\n", a.display(), b.display())).unwrap();
        let p = paths.clone();
        let (view, cx) = cx.add_window_view(move |_, cx| SettingsView::with_usb(p, Page::Hooks, false, cx));
        click(cx, "install-all");
        click(cx, "apply-hooks");
        view.read_with(cx, |v, _| {
            assert_eq!((v.installed_hooks(), v.unreadable_hooks(), v.hook_accounts()), (1, 1, 2));
            assert_eq!(v.pending_hooks(), 0, "the broken file can't be fixed by installing");
        });
        let _ = std::fs::remove_dir_all(&paths.home);
    }
}
