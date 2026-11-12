//! The Update page: what a newer app release brings (its notes, and those of
//! the releases between), the download, and "Skip this update". The sidebar
//! footer announces a release until it is skipped; Settings opens the page
//! either way.
//!
//! The download takes this install's package ([`AppPackage`]) to the
//! Downloads folder, checked against the release's `SHA256SUMS`, and hands it
//! to the OS. On x86_64 Linux the button opens a menu of the release's
//! packages (`.deb`, AppImage) to pick from, whatever the install. Elsewhere,
//! installs the release has no package for get a link to the release page
//! instead.

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;

use clawdmeter_core::{Paths, Settings};
use clawdmeter_flash::release::{self, AppPackage, ReleaseNotes};
use gpui::{
    anchored, deferred, div, point, prelude::*, px, AnyElement, Context, Corner, EventEmitter, FocusHandle,
    KeyDownEvent, SharedString, Window,
};

use super::settings::write_settings;
use super::theme::{Theme, R_M, R_S};
use super::widgets::{bar, button, card, heading, menu_button, notice, para, subheading, ButtonKind, Tone};
use super::{live_status, run_blocking, Navigate, REFRESH};
use crate::{ipc, Page};

/// Bytes of a download so far, shared with the thread fetching it.
#[derive(Debug, Default)]
struct Progress {
    done: AtomicU64,
    /// 0 while unknown.
    total: AtomicU64,
}

/// Where the download of a release's package stands.
#[derive(Debug, Clone)]
enum Download {
    /// Not asked for.
    Idle,
    /// On its way.
    Running(Arc<Progress>),
    /// Downloaded and verified.
    Ready(PathBuf),
    /// The download or its checksum failed.
    Failed,
}

/// The release notes for the page.
#[derive(Debug, Clone)]
enum Notes {
    /// Not asked for.
    Unloaded,
    /// Being fetched.
    Loading,
    /// Newest release first.
    Ready(Vec<ReleaseNotes>),
    /// GitHub didn't answer.
    Failed,
}

/// The Update page.
pub struct UpdateView {
    paths: Paths,
    /// The package that updates this install (on Linux, the one picked);
    /// `None` where the release has none, and the page then links to the
    /// release page unless there is a menu to pick from.
    package: Option<AppPackage>,
    /// Linux: the packages the download button offers in its menu.
    choices: &'static [AppPackage],
    /// The package this install came from, as far as it can be told: the
    /// menu marks it.
    detected: Option<AppPackage>,
    /// The menu is open.
    menu_open: bool,
    /// Focus for the open menu, so Escape closes it.
    menu_focus: FocusHandle,
    /// Only release builds are offered a release: a local build has no
    /// release version to compare (the firmware check skips `dev` boards
    /// likewise).
    offer: bool,
    /// "Check for updates" (`update_check`), as last read. Off, nothing is
    /// announced: the cache may still hold a release found before.
    checks: bool,
    /// `release_repo`, as last read.
    repo: String,
    /// The tag of a release newer than this app.
    latest: Option<String>,
    /// The release the user asked not to be told about (`skipped_update`).
    skipped: Option<String>,
    /// The download, and the release it is for.
    download: Download,
    download_tag: Option<String>,
    /// The notes, and the release they lead up to.
    notes: Notes,
    notes_tag: Option<String>,
    /// The page is on screen.
    active: bool,
    /// "Skip this update" couldn't be saved.
    skip_failed: bool,
    /// Refresh ticks since the files were last read.
    ticks: u32,
}

/// With the page off screen and no download running, the status and config
/// are re-read on every this many refresh ticks (5 s): the answer changes
/// once a day, and the overview already reads both files twice a second.
const IDLE_TICKS: u32 = 10;

impl EventEmitter<Navigate> for UpdateView {}

impl UpdateView {
    /// The page, following the update check's answer and the config.
    pub fn new(paths: Paths, cx: &mut Context<Self>) -> Self {
        cx.spawn(async move |this, cx| loop {
            cx.background_executor().timer(REFRESH).await;
            if this.update(cx, |v, cx| v.refresh(cx)).is_err() {
                break;
            }
        })
        .detach();
        let package = AppPackage::for_this_install();
        let mut v = UpdateView {
            paths,
            package,
            choices: AppPackage::linux_choices(),
            detected: package,
            menu_open: false,
            menu_focus: cx.focus_handle(),
            offer: crate::is_release_build(),
            checks: true,
            repo: clawdmeter_flash::DEFAULT_RELEASE_REPO.into(),
            latest: None,
            skipped: None,
            download: Download::Idle,
            download_tag: None,
            notes: Notes::Unloaded,
            notes_tag: None,
            active: false,
            skip_failed: false,
            ticks: 0,
        };
        v.reload();
        v
    }

    /// Re-read the update check's answer and the config; true on a change.
    fn reload(&mut self) -> bool {
        let (checks, repo, skipped) = match Settings::load(&self.paths) {
            // A hand-written `2.5.0` skips `v2.5.0` too.
            Ok(s) => (s.update_check, s.release_repo, s.skipped_update.as_deref().and_then(release::release_tag)),
            Err(_) => (self.checks, self.repo.clone(), self.skipped.clone()),
        };
        let latest = if self.offer && checks { newer_release(&self.paths) } else { None };
        let changed = latest != self.latest || skipped != self.skipped || repo != self.repo || checks != self.checks;
        self.checks = checks;
        if repo != self.repo {
            // Another repo's release of the same name is another release.
            self.notes = Notes::Unloaded;
            self.notes_tag = None;
            if !matches!(self.download, Download::Running(_)) {
                self.download = Download::Idle;
                self.download_tag = None;
            }
        }
        if latest != self.latest {
            // The menu belongs to the release it was opened for.
            self.menu_open = false;
        }
        self.latest = latest;
        self.skipped = skipped;
        self.repo = repo;
        changed
    }

    fn refresh(&mut self, cx: &mut Context<Self>) {
        let running = matches!(self.download, Download::Running(_));
        self.ticks += 1;
        let changed = (self.active || running || self.ticks >= IDLE_TICKS) && {
            self.ticks = 0;
            self.reload()
        };
        if self.active {
            self.load_notes(cx);
        }
        if changed || running {
            cx.notify();
        }
    }

    /// The tag of a release newer than this app, skipped or not.
    pub fn available(&self) -> Option<&str> {
        self.latest.as_deref()
    }

    /// The tag of a newer release the user hasn't skipped: the one the
    /// sidebar announces.
    pub fn announced(&self) -> Option<&str> {
        self.available().filter(|tag| self.skipped.as_deref() != Some(*tag))
    }

    /// The page came on screen or left it. The notes load when it shows.
    pub fn set_active(&mut self, active: bool, cx: &mut Context<Self>) {
        self.active = active;
        self.menu_open = false;
        if active {
            self.reload();
            // Coming back to the page asks again for notes that didn't load.
            if matches!(self.notes, Notes::Failed) {
                self.notes_tag = None;
            }
            self.skip_failed = false;
            self.load_notes(cx);
        }
        cx.notify();
    }

    /// Whether a newer release is offered at all (tests and the demo; by
    /// default only to release builds).
    pub fn offer(&mut self, on: bool) {
        self.offer = on;
        self.reload();
    }

    /// Pretend this install updates from `package`, with no menu to pick
    /// from (tests and the demo).
    pub fn set_package(&mut self, package: Option<AppPackage>) {
        self.package = package;
        self.choices = &[];
    }

    /// Pretend this install picks from the Linux packages `choices`, came
    /// from `detected`, and has the menu open or not (tests and the demo).
    pub fn set_choices(&mut self, choices: &'static [AppPackage], detected: Option<AppPackage>, open: bool) {
        self.package = detected;
        self.choices = choices;
        self.detected = detected;
        self.menu_open = open;
    }

    /// Whether the download button's menu is open (tests).
    pub fn menu_open(&self) -> bool {
        self.menu_open
    }

    fn open_menu(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.menu_open = true;
        window.focus(&self.menu_focus);
        cx.notify();
    }

    fn close_menu(&mut self, cx: &mut Context<Self>) {
        self.menu_open = false;
        cx.notify();
    }

    /// Download `package` of release `tag`, picked from the menu.
    fn pick(&mut self, package: AppPackage, tag: String, cx: &mut Context<Self>) {
        self.menu_open = false;
        self.package = Some(package);
        self.start_download(tag, cx);
    }

    /// Show these notes for release `tag` without asking GitHub (tests and
    /// the demo).
    pub fn show_notes(&mut self, tag: &str, notes: Vec<ReleaseNotes>) {
        self.notes_tag = Some(tag.to_owned());
        self.notes = Notes::Ready(notes);
    }

    /// Put the download of release `tag` in a given state without fetching
    /// anything (the demo): `downloading`, `ready`, `failed`, else idle.
    pub fn show_download_state(&mut self, tag: &str, state: &str) {
        self.download_tag = Some(tag.to_owned());
        self.download = match state {
            "downloading" => {
                Download::Running(Arc::new(Progress { done: AtomicU64::new(42), total: AtomicU64::new(100) }))
            }
            "ready" => Download::Ready(PathBuf::new()),
            "failed" => Download::Failed,
            _ => Download::Idle,
        };
    }

    /// Fetch the notes of the releases up to the newer one, once per release.
    fn load_notes(&mut self, cx: &mut Context<Self>) {
        let Some(tag) = self.latest.clone() else { return };
        if self.notes_tag.as_deref() == Some(tag.as_str()) {
            return;
        }
        self.notes_tag = Some(tag.clone());
        self.notes = Notes::Loading;
        let repo = self.repo.clone();
        let asked = tag.clone();
        run_blocking(
            cx,
            move || release::release_notes(&repo, crate::version(), &tag),
            move |v, r, _| {
                if v.notes_tag.as_deref() != Some(asked.as_str()) {
                    return;
                }
                v.notes = match r {
                    Ok(notes) => Notes::Ready(notes),
                    Err(e) => {
                        log::warn!("release notes: {e} ({})", e.detail().unwrap_or_default());
                        Notes::Failed
                    }
                };
            },
        );
    }

    /// Stop announcing this release and leave the page. A later release is
    /// announced again.
    fn skip(&mut self, cx: &mut Context<Self>) {
        let Some(tag) = self.latest.clone() else { return };
        if self.set_skipped(Some(tag)) {
            cx.emit(Navigate(Page::Overview));
        }
        cx.notify();
    }

    /// Save the skipped release; false (and a message on the page) when the
    /// config couldn't be written.
    fn set_skipped(&mut self, tag: Option<String>) -> bool {
        let saved = tag.clone();
        match write_settings(&self.paths, move |s, _| s.skipped_update = saved) {
            Ok(()) => {
                self.skipped = tag;
                self.skip_failed = false;
            }
            Err(e) => {
                log::warn!("couldn't save skipped_update: {e}");
                self.skip_failed = true;
            }
        }
        !self.skip_failed
    }

    /// Download the package of release `tag` to the Downloads folder, then
    /// hand it to the OS: an installer opens, any other package is shown in
    /// the file manager.
    fn start_download(&mut self, tag: String, cx: &mut Context<Self>) {
        let Some(package) = self.package else { return };
        if matches!(self.download, Download::Running(_)) {
            return;
        }
        let progress = Arc::new(Progress::default());
        self.download = Download::Running(progress.clone());
        self.download_tag = Some(tag.clone());
        let repo = self.repo.clone();
        // The Downloads folder, or the cache when there is none or it can't
        // be written (macOS asks before an app may use it).
        let downloads = self.paths.home.join("Downloads");
        let cache = self.paths.cache_dir.clone();
        run_blocking(
            cx,
            move || {
                let fetch = |dir: &Path| {
                    release::fetch_app_package(&repo, &tag, package, dir, |done, total| {
                        progress.done.store(done, Ordering::Relaxed);
                        progress.total.store(total, Ordering::Relaxed);
                    })
                };
                if !downloads.is_dir() {
                    return fetch(&cache);
                }
                match fetch(&downloads) {
                    Err(release::ReleaseError::Io(e)) => {
                        log::info!("app update: {}: {e}; using the cache", downloads.display());
                        fetch(&cache)
                    }
                    r => r,
                }
            },
            move |v, r, cx| match r {
                Ok(path) => {
                    log::info!("app update downloaded to {}", path.display());
                    // Not when a newer release turned up meanwhile: the page
                    // now offers that one, and this file stays in Downloads.
                    if v.latest == v.download_tag {
                        open_package(package, &path, cx);
                    }
                    v.download = Download::Ready(path);
                }
                Err(e) => {
                    log::warn!("app update download: {e} ({})", e.detail().unwrap_or_default());
                    v.download = Download::Failed;
                }
            },
        );
        cx.notify();
    }

    /// The download button and what it is doing, for release `tag`.
    fn download_block(&self, t: &Theme, tag: &str, window: &Window, cx: &mut Context<Self>) -> gpui::Div {
        let version = tag.trim_start_matches('v').to_owned();
        let page_url = release::release_page_url(&self.repo, tag);
        // A download belongs to one release and a newer one starts over, but
        // only once the one on its way has landed.
        let same = self.download_tag.as_deref() == Some(tag);
        let state =
            if same || matches!(self.download, Download::Running(_)) { self.download.clone() } else { Download::Idle };
        // The version on its way, which an earlier release's download makes
        // an older one than the page offers.
        let coming = self.download_tag.as_deref().unwrap_or(tag).trim_start_matches('v').to_owned();
        let package = self.package;
        // Linux picks a package from the button's menu; once one is on its
        // way or downloaded, the button follows that one. After a failure
        // the menu is back, so another package can be tried.
        let menu = !self.choices.is_empty() && matches!(state, Download::Idle | Download::Failed);
        let label: SharedString = match (&state, package) {
            (Download::Failed, _) if menu => "Try again".into(),
            _ if menu => format!("Download {version}").into(),
            (_, None) => format!("Get {version} on GitHub").into(),
            (Download::Ready(_), Some(p)) if p.opens_installer() => "Open installer".into(),
            (Download::Ready(_), _) => "Show download".into(),
            (Download::Failed, _) => "Try again".into(),
            (Download::Running(_), _) => format!("Download {coming}").into(),
            _ => format!("Download {version}").into(),
        };
        let running = match &state {
            Download::Running(p) => Some((p.done.load(Ordering::Relaxed), p.total.load(Ordering::Relaxed))),
            _ => None,
        };
        let failed = matches!(state, Download::Failed) && package.is_some();
        let skipped = self.skipped.as_deref() == Some(tag);
        let owned = tag.to_owned();
        let url = page_url.clone();
        let on_click = cx.listener(move |v, _, window, cx| match (&state, v.package) {
            _ if menu => v.open_menu(window, cx),
            (_, None) => cx.open_url(&url),
            (Download::Ready(path), Some(p)) if path.exists() => open_package(p, path, cx),
            _ => v.start_download(owned.clone(), cx),
        });
        let main: AnyElement = if menu {
            div()
                .child(menu_button(t, "update-download", Some("linux"), label, ButtonKind::Primary, true, on_click))
                .when(self.menu_open, |d| d.children(self.package_menu(t, tag, window, cx)))
                .into_any_element()
        } else {
            button(t, "update-download", label, ButtonKind::Primary, running.is_none(), on_click).into_any_element()
        };
        let mut row = div().flex().items_center().flex_wrap().gap(px(10.)).child(main);
        row = if skipped {
            row.child(button(
                t,
                "update-unskip",
                "Tell me about this update",
                ButtonKind::Normal,
                true,
                cx.listener(|v, _, _, cx| {
                    v.set_skipped(None);
                    cx.notify();
                }),
            ))
        } else {
            row.child(button(
                t,
                "update-skip",
                "Skip this update",
                ButtonKind::Normal,
                // Not while its download runs: that ends by opening it.
                running.is_none(),
                cx.listener(|v, _, _, cx| v.skip(cx)),
            ))
        };
        // With no package the button already goes to the release page.
        if package.is_some() || !self.choices.is_empty() {
            row = row.child(
                div()
                    .id("update-page-link")
                    .debug_selector(|| "update-page-link".into())
                    .px(px(6.))
                    .text_size(px(13.5))
                    .text_color(t.dim)
                    .cursor_pointer()
                    .hover(|s| s.text_color(t.text).underline())
                    .on_click(move |_, _, cx| cx.open_url(&page_url))
                    .child("View on GitHub"),
            );
        }
        let mut block = div().flex().flex_col().gap(px(12.)).child(row);
        if let Some((done, total)) = running {
            let frac = if total == 0 { 0. } else { (done as f32 / total as f32).clamp(0., 1.) };
            let text = if total == 0 {
                format!("Downloading {coming}")
            } else {
                format!("Downloading {coming}, {:.0}%", frac * 100.)
            };
            block = block.child(
                div()
                    .flex()
                    .flex_col()
                    .gap(px(6.))
                    .max_w(px(360.))
                    .child(div().text_size(px(13.)).text_color(t.dim).child(text))
                    .child(bar(t, frac, t.accent)),
            );
        }
        if failed {
            block = block.child(notice(t, Tone::Bad, "Couldn't download it. Try again, or get it from GitHub."));
        }
        if self.skip_failed {
            block = block.child(notice(t, Tone::Bad, "Couldn't save that to the config file."));
        }
        block
    }

    /// The download button's menu, under the button: the release's Linux
    /// packages, the one this install came from marked. A click outside it
    /// or Escape closes it.
    fn package_menu(&self, t: &Theme, tag: &str, window: &Window, cx: &mut Context<Self>) -> [AnyElement; 2] {
        // A clear layer over the window takes the click that closes the
        // menu, so that click doesn't also land on what is under it.
        let viewport = window.viewport_size();
        let backdrop = deferred(
            anchored().position(point(px(0.), px(0.))).child(
                div()
                    .id("update-menu-backdrop")
                    .debug_selector(|| "update-menu-backdrop".into())
                    .w(viewport.width)
                    .h(viewport.height)
                    .occlude()
                    .on_any_mouse_down(cx.listener(|v, _, _, cx| v.close_menu(cx))),
            ),
        )
        .with_priority(1);
        let hover = t.hover;
        let items = self.choices.iter().map(|&p| {
            let (id, name, hint) = match p {
                AppPackage::Deb => ("update-menu-deb", ".deb", "Debian, Ubuntu"),
                AppPackage::AppImage => ("update-menu-appimage", "AppImage", "Any distribution"),
                AppPackage::Dmg => ("update-menu-dmg", ".dmg", "macOS"),
                AppPackage::Msi => ("update-menu-msi", ".msi", "Windows"),
            };
            let (hint, hint_color) = if self.detected == Some(p) { ("This install", t.accent) } else { (hint, t.mute) };
            let owned = tag.to_owned();
            div()
                .id(id)
                .debug_selector(move || id.into())
                .flex()
                .items_center()
                .justify_between()
                .gap(px(24.))
                .h(px(34.))
                .px(px(10.))
                .rounded(px(R_S))
                .cursor_pointer()
                .hover(move |s| s.bg(hover))
                .child(div().text_size(px(13.5)).text_color(t.text).child(name))
                .child(div().text_size(px(12.5)).text_color(hint_color).child(hint))
                .on_click(cx.listener(move |v, _, _, cx| v.pick(p, owned.clone(), cx)))
        });
        let menu = deferred(
            // Laid out after the button, so this is the gap under it.
            anchored().anchor(Corner::TopLeft).offset(point(px(0.), px(4.))).snap_to_window_with_margin(px(8.)).child(
                div()
                    .id("update-menu")
                    .track_focus(&self.menu_focus)
                    .on_key_down(cx.listener(|v, e: &KeyDownEvent, _, cx| {
                        if e.keystroke.key == "escape" {
                            v.close_menu(cx);
                        }
                    }))
                    .occlude()
                    .flex()
                    .flex_col()
                    .min_w(px(260.))
                    .p(px(4.))
                    .rounded(px(R_M))
                    .border_1()
                    .border_color(t.line)
                    .bg(t.surface)
                    .shadow_lg()
                    .children(items),
            ),
        )
        .with_priority(2);
        [backdrop.into_any_element(), menu.into_any_element()]
    }

    /// The release notes, one card per release.
    fn notes_block(&self, t: &Theme) -> gpui::Div {
        let list = div().flex().flex_col().gap(px(12.));
        match &self.notes {
            Notes::Unloaded | Notes::Loading => list.child(para(t, "Loading the release notes.")),
            Notes::Failed => list.child(para(t, "Couldn't load the release notes. They are on the GitHub page.")),
            Notes::Ready(notes) if notes.is_empty() => {
                list.child(para(t, "No release notes were found. See the GitHub page."))
            }
            Notes::Ready(notes) => list.children(notes.iter().map(|n| release_card(t, n))),
        }
    }
}

/// The release newer than this app that the update check last saw: from the
/// tray's status while it runs, else from the check's cache file.
fn newer_release(paths: &Paths) -> Option<String> {
    let status = live_status(paths).unwrap_or_else(|| ipc::StatusSnapshot {
        latest_firmware: ipc::load_latest_release(paths),
        ..Default::default()
    });
    status.app_update(crate::version())
}

/// Hand a downloaded package to the OS: mount the disk image or start the
/// installer; show a `.deb` or AppImage in the file manager.
fn open_package(package: AppPackage, path: &Path, cx: &mut gpui::App) {
    if package.opens_installer() {
        cx.open_with_system(path);
    } else {
        cx.reveal_path(path);
    }
}

/// A line of release notes, as far as the page draws Markdown.
#[derive(Debug, Clone, PartialEq, Eq)]
enum NoteLine {
    /// `## Fixed`.
    Heading(String),
    /// `- item`, and how deep it is nested.
    Bullet(String, usize),
    /// A paragraph line.
    Text(String),
}

/// Most lines of one release's notes the page draws; the rest are on GitHub.
const MAX_NOTE_LINES: usize = 60;

/// Release notes as lines to draw: headings, bullets and plain text, with
/// the inline Markdown ([`plain`]) taken out. HTML comments, rules and blank
/// lines are dropped.
fn note_lines(body: &str) -> Vec<NoteLine> {
    let mut lines = Vec::new();
    let mut comment = false;
    for raw in body.lines() {
        let line = raw.trim_end();
        let text = line.trim_start();
        if comment {
            comment = !text.contains("-->");
            continue;
        }
        if text.starts_with("<!--") {
            comment = !text.contains("-->");
            continue;
        }
        if text.is_empty() || text.starts_with("```") || text.chars().all(|c| matches!(c, '-' | '*' | '_' | ' ')) {
            continue;
        }
        let indent = line.len() - text.len();
        let text = text.strip_prefix("> ").unwrap_or(text);
        let heading = text.trim_start_matches('#').strip_prefix(' ').filter(|_| text.starts_with('#'));
        let item = if let Some(rest) = heading {
            NoteLine::Heading(plain(rest))
        } else if let Some(rest) = ["- ", "* ", "+ "].iter().find_map(|m| text.strip_prefix(m)) {
            NoteLine::Bullet(plain(rest), (indent / 2).min(3))
        } else {
            NoteLine::Text(plain(text))
        };
        lines.push(item);
    }
    lines
}

/// Inline Markdown as plain text: `[text](url)` keeps its text, and emphasis
/// and code marks go.
fn plain(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut rest = s.trim();
    // Each `](` closes a link whose text starts at the nearest `[` before it.
    while let Some(mid) = rest.find("](") {
        let (Some(open), Some(end)) = (rest[..mid].rfind('['), rest[mid..].find(')')) else {
            // Not a link: keep it and look at what follows.
            out.push_str(&rest[..mid + 2]);
            rest = &rest[mid + 2..];
            continue;
        };
        // `![alt](image)` keeps its alt text.
        out.push_str(rest[..open].strip_suffix('!').unwrap_or(&rest[..open]));
        out.push_str(&rest[open + 1..mid]);
        rest = &rest[mid + end + 1..];
    }
    out.push_str(rest);
    out.replace("**", "").replace("__", "").replace('`', "")
}

/// One release: its version, the day it came out, and its notes.
fn release_card(t: &Theme, notes: &ReleaseNotes) -> impl IntoElement {
    let lines = note_lines(&notes.body);
    let more = lines.len() > MAX_NOTE_LINES;
    let mut body = div().flex().flex_col().gap(px(5.));
    for line in lines.into_iter().take(MAX_NOTE_LINES) {
        let text = |s: String| div().flex_1().min_w_0().text_size(px(13.5)).line_height(px(20.)).child(s);
        body = body.child(match line {
            NoteLine::Heading(s) => div().pt(px(6.)).text_size(px(13.5)).text_color(t.text).child(s),
            NoteLine::Text(s) => div().flex().text_color(t.dim).child(text(s)),
            NoteLine::Bullet(s, depth) => div()
                .flex()
                .gap(px(8.))
                .pl(px(depth as f32 * 16.))
                .text_color(t.dim)
                .child(div().flex_shrink_0().text_size(px(13.5)).line_height(px(20.)).text_color(t.mute).child("•"))
                .child(text(s)),
        });
    }
    if more {
        body = body.child(div().pt(px(4.)).text_size(px(13.)).text_color(t.mute).child("More on GitHub."));
    }
    let title = div()
        .flex()
        .items_center()
        .justify_between()
        .child(subheading(t, notes.tag.trim_start_matches('v').to_owned()))
        .child(div().text_size(px(12.5)).text_color(t.mute).child(notes.published.clone()));
    card(t).child(title).child(body)
}

impl Render for UpdateView {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let t = Theme::for_appearance(window.appearance());
        let page = div().flex_1().flex().flex_col().gap(px(20.));
        let Some(tag) = self.latest.clone() else {
            let detail = if !self.offer {
                format!("Version {} is a local build, so it isn't offered releases.", crate::version())
            } else if !self.checks {
                "\"Check for updates\" is off in Settings, so the app doesn't look for releases.".to_owned()
            } else {
                format!("The daily check has found no release newer than {}.", crate::version())
            };
            return page.child(
                div()
                    .flex()
                    .flex_col()
                    .gap(px(6.))
                    .child(heading(
                        &t,
                        if self.offer && self.checks { "No update available" } else { "Updates are off" },
                    ))
                    .child(para(&t, detail)),
            );
        };
        let version = tag.trim_start_matches('v');
        let mut detail = format!("You have {}.", crate::version());
        if self.skipped.as_deref() == Some(tag.as_str()) {
            detail.push_str(" You skipped this update, so the sidebar doesn't mention it.");
        }
        page.child(
            div()
                .flex()
                .flex_col()
                .gap(px(6.))
                .child(heading(&t, format!("Clawdmeter {version} is available")))
                .child(para(&t, detail)),
        )
        .child(self.download_block(&t, &tag, window, cx))
        .child(self.notes_block(&t))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn notes_become_lines() {
        let body = "<!-- generated -->\n## What's Changed\n\n* Fix the **meter** by @a in [#12](https://x/12)\n  - nested `code`\n\n---\nPlain line\n<!--\nhidden\n-->\n**Full Changelog**: v1...v2";
        assert_eq!(
            note_lines(body),
            [
                NoteLine::Heading("What's Changed".into()),
                NoteLine::Bullet("Fix the meter by @a in #12".into(), 0),
                NoteLine::Bullet("nested code".into(), 1),
                NoteLine::Text("Plain line".into()),
                NoteLine::Text("Full Changelog: v1...v2".into()),
            ]
        );
        assert_eq!(plain("a [b](c) d [e](f)"), "a b d e");
        assert_eq!(plain("[Desktop] Fix ([#12](u)) ![shot](s.png)"), "[Desktop] Fix (#12) shot");
        assert_eq!(plain("é [ü](x) ](  ) ["), "é ü ](  ) [");
        assert_eq!(
            note_lines("#262 fixed\n> quoted\n```rust\nlet a = 1;\n```"),
            [NoteLine::Text("#262 fixed".into()), NoteLine::Text("quoted".into()), NoteLine::Text("let a = 1;".into()),]
        );
        assert_eq!(plain("an [open bracket"), "an [open bracket");
        assert_eq!(plain("see ](x) and [#12](u)"), "see ](x) and #12", "a stray `](` doesn't end it");
        assert_eq!(plain("a ]( b [c](d) ]("), "a ]( b c ](");
        assert!(note_lines("\n\n").is_empty());
    }
}
